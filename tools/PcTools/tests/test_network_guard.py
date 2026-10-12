"""Tests of conftest's _no_outbound_network guard (review confx HIGH-1)."""
import socket

import pytest

from conftest import NetworkGuardError, _is_loopback_addr


def test_loopback_classification():
    assert _is_loopback_addr(("127.0.0.1", 80))
    assert _is_loopback_addr(("::1", 80, 0, 0))
    assert _is_loopback_addr(("localhost", 80))
    assert _is_loopback_addr("/tmp/unix.sock")
    assert not _is_loopback_addr(("192.168.1.156", 80))
    assert not _is_loopback_addr(("10.0.0.5", 80))
    assert not _is_loopback_addr(("bench.example", 80))


@pytest.mark.parametrize("how", ["connect", "connect_ex", "create_connection"])
def test_guard_refuses_and_records_non_loopback(how, _no_outbound_network):
    addr = ("192.168.1.156", 80)
    with pytest.raises(NetworkGuardError):
        if how == "create_connection":
            socket.create_connection(addr, timeout=0.2)
        else:
            s = socket.socket()
            try:
                getattr(s, how)(addr)
            finally:
                s.close()
    assert len(_no_outbound_network) == 1
    # Consume the record: teardown would otherwise (correctly) fail this test.
    _no_outbound_network.clear()


def test_swallowed_refusal_still_fails_teardown():
    """A caller that swallows the exception must not hide the attempt."""
    import subprocess, sys, textwrap, os
    here = os.path.dirname(__file__)
    code = textwrap.dedent("""
        import socket
        def test_swallows():
            try:
                socket.create_connection(("10.0.0.5", 80), timeout=0.2)
            except Exception:
                pass
    """)
    import tempfile, shutil
    d = tempfile.mkdtemp()
    try:
        shutil.copy(os.path.join(here, "conftest.py"), d)
        shutil.copy(os.path.join(here, "bench_fixture_session.py"), d)
        open(os.path.join(d, "test_x.py"), "w").write(code)
        r = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                            "-p", "no:xdist", d], capture_output=True, text=True, cwd=d,
                           env={**os.environ, "PYTHONPATH": os.path.join(here, "..", "src")})
    finally:
        shutil.rmtree(d, ignore_errors=True)
    assert r.returncode != 0, r.stdout + r.stderr
    assert "outbound network" in r.stdout


def test_loopback_still_allowed():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    try:
        c = socket.create_connection(srv.getsockname(), timeout=2)
        c.close()
    finally:
        srv.close()


def test_guard_refuses_link_hub_port_on_loopback(_no_outbound_network):
    from kilnctrl.link_hub import HUB_PORT
    with pytest.raises(NetworkGuardError):
        socket.create_connection(("127.0.0.1", HUB_PORT), timeout=0.2)
    assert len(_no_outbound_network) == 1
    _no_outbound_network.clear()


def test_get_shared_link_disabled_under_pytest():
    from kilnctrl import link_hub
    with pytest.raises(link_hub.LinkHubDisabled):
        link_hub.get_shared_link()


def test_mcp_server_import_link_is_null_link():
    from kilnctrl import mcp_server
    assert mcp_server._link is not None
    from kilnctrl import link_hub
    assert isinstance(mcp_server._link, link_hub.NullLink)
    assert not mcp_server._link.is_connected


def test_guard_refuses_udp_sendto(_no_outbound_network):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        with pytest.raises(NetworkGuardError):
            s.sendto(b"x", ("192.0.2.1", 9))
    finally:
        s.close()
    _no_outbound_network.clear()


def test_udp_sendto_loopback_allowed():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.sendto(b"x", ("127.0.0.1", 9))
    finally:
        s.close()


def test_proxy_env_removed_inside_guarded_test():
    """LOW-1: a proxy variable set in the outer environment is deleted per test."""
    import subprocess, sys, textwrap, os, tempfile, shutil
    here = os.path.dirname(__file__)
    code = textwrap.dedent("""
        import os
        def test_x():
            for v in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
                assert v not in os.environ, v
            # NO_PROXY=* keeps urllib off the Windows registry proxy (LOW-1)
            assert os.environ.get("NO_PROXY") == "*" and os.environ.get("no_proxy") == "*"
            import urllib.request
            assert urllib.request.getproxies() == {"no": "*"}, urllib.request.getproxies()
    """)
    d = tempfile.mkdtemp()
    try:
        shutil.copy(os.path.join(here, "conftest.py"), d)
        shutil.copy(os.path.join(here, "bench_fixture_session.py"), d)
        open(os.path.join(d, "test_x.py"), "w").write(code)
        env = {**os.environ, "PYTHONPATH": os.path.join(here, "..", "src"),
               "HTTP_PROXY": "http://127.0.0.1:1", "HTTPS_PROXY": "http://127.0.0.1:1",
               "ALL_PROXY": "http://127.0.0.1:1", "http_proxy": "http://127.0.0.1:1"}
        r = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                            "-p", "no:xdist", d], capture_output=True, text=True, cwd=d, env=env)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.mark.live_bench
def test_live_bench_marker_alone_does_not_exempt(_no_outbound_network):
    """LOW-2: without KILNCTRL_BENCH_HOST the marker grants nothing."""
    import os
    if os.environ.get("KILNCTRL_BENCH_HOST"):
        pytest.skip("bench host selected: exemption legitimately applies")
    with pytest.raises(NetworkGuardError):
        socket.create_connection(("192.0.2.1", 80), timeout=0.2)
    _no_outbound_network.clear()


def _run_child_pytest(code, extra_env, drop=()):
    import subprocess, sys, tempfile, shutil, os
    here = os.path.dirname(__file__)
    d = tempfile.mkdtemp()
    try:
        shutil.copy(os.path.join(here, "conftest.py"), d)
        shutil.copy(os.path.join(here, "bench_fixture_session.py"), d)
        open(os.path.join(d, "test_x.py"), "w").write(code)
        env = {k: v for k, v in os.environ.items()
               if k not in drop and k not in ("KILNCTRL_BENCH_HOST", "KILNCTL_TEST_NO_HUB")}
        env.update(extra_env)
        env["PYTHONPATH"] = os.path.join(here, "..", "src")
        return subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                               "-p", "no:xdist", d], capture_output=True, text=True, cwd=d, env=env)
    finally:
        shutil.rmtree(d, ignore_errors=True)


_CRED = {"KILNCTL_HOST": "10.9.9.9", "KILNCTL_WEB_USERNAME": "u",
         "KILNCTL_WEB_PASSWORD": "p", "KILNCTL_WEB_PASSWORD_NEW": "q"}
# Names live tests use: kept when a bench host is set (conftest._BENCH_KEEP).
_KEPT = ("KILNCTL_HOST", "KILNCTL_WEB_USERNAME", "KILNCTL_WEB_PASSWORD")
# Testnet3 L1: secrets/gates NOT in the old enumerated list, plus the other spellings.
_OTHER_SECRETS = {
    "KILNCTL_LCD_PIN": "1", "KILNCTL_STA_SSID": "s", "KILNCTL_STA_PASSWORD": "s",
    "KILNCTL_HOTSPOT_SSID": "s", "KILNCTL_HOTSPOT_PASSWORD": "s", "KILNCTL_AP_PASSWORD": "s",
    "KILNCTL_AUX_BENCH_CONFIRM": "1", "KILNCTL_WEB_USER_USERNAME": "s",
    "KILNCTL_WEB_USER_PASSWORD": "s", "KILNCTL_TOTP_CODE": "1", "KILNCTL_WIFI_PASSWORD": "s",
    "KILNCTL_BOARD_HOST": "s", "KILNCTL_RECOVERY_AP_PASSPHRASE": "s",
    "KILNCTL_GITHUB_TOKEN": "s", "KILNCTL_FUTURE_NEW_SECRET": "s",
    "MOUSER_API_KEY": "s", "GITHUB_TOKEN": "s"}
_CONTROL = {"KILNCTL_TEST_FOO": "1", "KILNCTL_SLOW_TESTS": "1"}


def test_conftest_drops_board_env_without_bench_host():
    """LOW-3/L1: no bench host -> children inherit no board address or secret."""
    code = ("import os\ndef test_x():\n"
            "    for v in %r:\n        assert v not in os.environ, v\n"
            "    assert os.environ['KILNCTL_TEST_NO_HUB'] == '1'\n"
            "    assert os.environ['KILNCTL_TEST_FOO'] == '1'\n"
            "    assert os.environ['KILNCTL_SLOW_TESTS'] == '1'\n"
            % (tuple(_CRED) + tuple(_OTHER_SECRETS),))
    r = _run_child_pytest(code, {**_CRED, **_OTHER_SECRETS, **_CONTROL})
    assert r.returncode == 0, r.stdout + r.stderr


def test_conftest_keeps_board_env_with_bench_host_but_no_hub():
    """LOW-3/LOW-2/L2: a bench host keeps exactly what live tests use (every kept name
    asserted, with its value), drops every other secret, and the hub stays disabled."""
    code = ("import os\ndef test_x():\n"
            "    kept = %r\n"
            "    for v, val in kept.items():\n        assert os.environ[v] == val, v\n"
            "    assert os.environ['KILNCTRL_BENCH_HOST'] == '10.9.9.9'\n"
            "    for v in %r:\n        assert v not in os.environ, v\n"
            "    assert os.environ['KILNCTL_TEST_NO_HUB'] == '1'\n"
            "    assert os.environ['KILNCTL_TEST_FOO'] == '1'\n"
            % ({k: _CRED[k] for k in _KEPT},
               ("KILNCTL_WEB_PASSWORD_NEW",) + tuple(_OTHER_SECRETS)))
    r = _run_child_pytest(code, {**_CRED, **_OTHER_SECRETS, **_CONTROL,
                                 "KILNCTRL_BENCH_HOST": "10.9.9.9"})
    assert r.returncode == 0, r.stdout + r.stderr


def test_conftest_forces_no_hub_over_inherited_zero():
    """I2: an inherited KILNCTL_TEST_NO_HUB=0 must read back "1" (set, not setdefault)."""
    code = "import os\ndef test_x():\n    assert os.environ['KILNCTL_TEST_NO_HUB'] == '1'\n"
    import subprocess, sys, tempfile, shutil, os
    here = os.path.dirname(__file__)
    d = tempfile.mkdtemp()
    try:
        shutil.copy(os.path.join(here, "conftest.py"), d)
        shutil.copy(os.path.join(here, "bench_fixture_session.py"), d)
        open(os.path.join(d, "test_x.py"), "w").write(code)
        env = {k: v for k, v in os.environ.items() if k != "KILNCTRL_BENCH_HOST"}
        env["KILNCTL_TEST_NO_HUB"] = "0"
        env["PYTHONPATH"] = os.path.join(here, "..", "src")
        r = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                            "-p", "no:xdist", d], capture_output=True, text=True, cwd=d, env=env)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_nulllink_fallback_warning_logged():
    """I1: importing mcp_server with the hub disabled logs the NullLink fallback warning."""
    import subprocess, sys, os
    here = os.path.dirname(__file__)
    code = ("import logging, io, sys\n"
            "buf = io.StringIO()\n"
            "logging.basicConfig(stream=buf, level=logging.WARNING)\n"
            "import kilnctrl.mcp_server\n"
            "sys.stdout.write('CAPTURED:' + buf.getvalue())\n")
    env = {**os.environ, "KILNCTL_TEST_NO_HUB": "1",
           "PYTHONPATH": os.path.join(here, "..", "src")}
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "link hub disabled, using NullLink" in r.stdout, r.stdout + r.stderr


def _ps_exe():
    import shutil
    return shutil.which("powershell") or shutil.which("pwsh")


def _repo_file(*parts):
    import os
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", *parts))


_PROBE = "GUARD-PROBE-PAST-GUARD"


@pytest.mark.skipif(_ps_exe() is None, reason="needs powershell")
@pytest.mark.parametrize("script_parts,args", [
    (("tools", "PcTools", "scripts", "web_commission_row.ps1"), ["-Row", "W28", "-BoardHost", "192.0.2.1", "-ScreenshotDir", "unused"]),
    (("tools", "PcTools", "scripts", "bench_hotspot.ps1"), ["-Action", "start"]),
    (("tools", "PcTools", "scripts", "bench_hotspot.ps1"), ["-Action", "stop"]),
    (("tools", "PcTools", "scripts", "bench_hotspot.ps1"), ["-Action", "status"]),
    (("tools", "make_release.ps1"), ["-Tag", "v0.0.0-test.1", "-Publish"]),
    (("tools", "make_release.ps1"), ["-Tag", "v0.0.0-test.1", "-DevDryRun"]),
])
def test_user_scope_credential_scripts_refuse_under_test_env(script_parts, args):
    """Testnet3 L3 / testnet4 L2+L3: these scripts use User-scope credentials or the real
    hotspot / git credential manager, which the conftest env drop cannot hide. Under
    KILNCTL_TEST_NO_HUB=1 each must refuse before the action.

    Safe on failure: KILNCTL_TEST_GUARD_PROBE=1 makes every script exit 0 printing the probe
    marker just PAST its guard, so a regressed guard cannot start the hotspot, publish, fetch
    or load credentials. The marker's absence plus a nonzero exit proves the guard fired first.
    (No sentinel-secret assertion: the scripts read the User-scope registry, never the process
    env, so a process-env sentinel could not leak and the check would be vacuous.)"""
    import subprocess, os
    env = {**os.environ, "KILNCTL_TEST_NO_HUB": "1", "KILNCTL_TEST_GUARD_PROBE": "1"}
    r = subprocess.run([_ps_exe(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        _repo_file(*script_parts), *args],
                       capture_output=True, text=True, env=env, timeout=120)
    out = r.stdout + r.stderr
    assert _PROBE not in out, "guard regressed (stopped only by the probe): " + out
    assert r.returncode != 0, out
    assert "refused" in out.lower() and "KILNCTL_TEST_NO_HUB" in out, out


@pytest.mark.skipif(_ps_exe() is None, reason="needs powershell")
def test_guard_probe_reaches_past_guard_without_no_hub():
    """Control for the test above: with the guard condition off, the probe marker appears, so
    the refusal test cannot pass merely because the probe path is dead."""
    import subprocess, os
    env = {k: v for k, v in os.environ.items() if k != "KILNCTL_TEST_NO_HUB"}
    env["KILNCTL_TEST_GUARD_PROBE"] = "1"
    r = subprocess.run([_ps_exe(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        _repo_file("tools", "PcTools", "scripts", "bench_hotspot.ps1"),
                        "-Action", "stop"], capture_output=True, text=True, env=env, timeout=120)
    assert r.returncode == 0 and _PROBE in r.stdout, r.stdout + r.stderr


def test_settings_override_hides_real_host_in_process(tmp_path):
    """Testnet4 L1: conftest points settings at a temp file, so a real-looking settings.json
    planted elsewhere cannot supply last_host. resolve_default_host() falls back."""
    import json, os
    from kilnctrl import host_resolve, settings
    assert os.environ["KILNCTL_TEST_SETTINGS_PATH"] == str(settings.SETTINGS_PATH)
    assert "kilnctl_test_settings_" in str(settings.SETTINGS_PATH)
    os.environ.pop("KILNCTL_HOST", None)
    assert settings.get_last_host() is None
    planted = tmp_path / "settings.json"
    planted.write_text(json.dumps({"last_host": "192.168.1.156", "last_port": "COM14"}))
    assert settings.get_last_host() is None and settings.get_last_port() is None
    assert host_resolve.resolve_default_host() == host_resolve.FALLBACK_HOST


def test_settings_override_inherited_by_subprocess():
    """Testnet4 L1: a child process (no conftest) also sees the override, so its settings path
    is the temp file and it resolves the fallback host."""
    import subprocess, sys, os
    here = os.path.dirname(__file__)
    code = ("import os\n"
            "from kilnctrl import host_resolve, settings\n"
            "os.environ.pop('KILNCTL_HOST', None)\n"
            "print('PATH=' + str(settings.SETTINGS_PATH))\n"
            "print('HOST=' + host_resolve.resolve_default_host())\n"
            "print('FALLBACK=' + host_resolve.FALLBACK_HOST)\n")
    env = {**os.environ, "PYTHONPATH": os.path.join(here, "..", "src")}
    assert env.get("KILNCTL_TEST_SETTINGS_PATH")
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    lines = dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l)
    assert lines["PATH"] == env["KILNCTL_TEST_SETTINGS_PATH"], r.stdout
    assert lines["HOST"] == lines["FALLBACK"], r.stdout


def test_scrub_keep_prefix_boundary():
    """Testnet4 I2: only the exact control prefix KILNCTL_TEST_ is kept; a name that merely
    starts with KILNCTL_TEST (no underscore) is secret-shaped and is dropped."""
    import conftest
    env = {"KILNCTL_TESTPASSWORD": "x", "KILNCTL_TEST_FLAG": "1", "KILNCTL_BUILDER_TOKEN": "x",
           "KILNCTL_BUILD_FLAG": "1"}
    conftest._scrub_board_env(env, False)
    assert env == {"KILNCTL_TEST_FLAG": "1", "KILNCTL_BUILD_FLAG": "1"}
