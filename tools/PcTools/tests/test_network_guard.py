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
