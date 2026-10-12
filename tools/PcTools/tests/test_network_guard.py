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


def test_conftest_drops_board_env_without_bench_host():
    """LOW-3: no bench host -> children cannot inherit the board address/credentials."""
    code = ("import os\ndef test_x():\n"
            "    for v in %r:\n        assert v not in os.environ, v\n"
            "    assert os.environ['KILNCTL_TEST_NO_HUB'] == '1'\n" % (tuple(_CRED),))
    r = _run_child_pytest(code, _CRED)
    assert r.returncode == 0, r.stdout + r.stderr


def test_conftest_keeps_board_env_with_bench_host_but_no_hub():
    """LOW-3/LOW-2: a selected bench host keeps the env, and the hub stays disabled."""
    code = ("import os\ndef test_x():\n"
            "    assert os.environ['KILNCTL_HOST'] == '10.9.9.9'\n"
            "    assert os.environ['KILNCTL_TEST_NO_HUB'] == '1'\n")
    r = _run_child_pytest(code, {**_CRED, "KILNCTRL_BENCH_HOST": "10.9.9.9"})
    assert r.returncode == 0, r.stdout + r.stderr
