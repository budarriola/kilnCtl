"""Tests of conftest's _no_outbound_network guard (review confx HIGH-1)."""
import re
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


@pytest.mark.parametrize("port", [8766, 8767])
def test_guard_refuses_mcp_server_ports_on_loopback(port, _no_outbound_network):
    """Testnet4 I1: loopback to the running MCP servers is refused."""
    with pytest.raises(NetworkGuardError):
        socket.create_connection(("127.0.0.1", port), timeout=0.2)
    assert len(_no_outbound_network) == 1
    _no_outbound_network.clear()


def test_scrub_drops_reuse_sid():
    """Testnet4 I3: KC_REUSE_SID (a web session cookie) is not KILNCTL_-prefixed but is dropped."""
    import conftest
    env = {"KC_REUSE_SID": "abc", "KILNCTL_TEST_FLAG": "1"}
    conftest._scrub_board_env(env, False)
    assert env == {"KILNCTL_TEST_FLAG": "1"}


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
    """Windows PowerShell by ABSOLUTE path under %SystemRoot% on win32 (never PATH, so a
    stripped PATH cannot make the guard pins silently skip; testnetfx9 LOW-2); pwsh/powershell
    from PATH elsewhere. None only when it is genuinely absent."""
    import os, shutil, sys
    if sys.platform == "win32":
        root = os.environ.get("SystemRoot") or os.environ.get("windir") or r"C:\Windows"
        p = os.path.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
        return p if os.path.isfile(p) else None
    return shutil.which("pwsh") or shutil.which("powershell")


def _need_ps():
    """The PowerShell exe, or skip off Windows / FAIL on Windows when it is missing."""
    import sys
    ps = _ps_exe()
    if ps is None:
        if sys.platform == "win32":
            pytest.fail("Windows PowerShell not found under %SystemRoot%\\System32\\WindowsPowerShell\\v1.0")
        pytest.skip("needs powershell (PSParser tokenizer)")
    return ps


def _repo_file(*parts):
    import os
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", *parts))


_PROBE = "GUARD-PROBE-PAST-GUARD"
_BACKSTOP = "GUARD-PROBE-CRED-BACKSTOP"


_TOKENIZE_PS = (
    "$t = Get-Content -LiteralPath $env:KILNCTL_TOK_FILE -Raw -Encoding UTF8; "
    "$e = $null; "
    "$tk = [System.Management.Automation.PSParser]::Tokenize($t, [ref]$e); "
    "$o = @($tk | ForEach-Object { [pscustomobject]@{ T = [string]$_.Type; C = [string]$_.Content; "
    "S = $_.Start; L = $_.Length; N = $_.StartLine } }); "
    "$er = @($e | ForEach-Object { [string]$_.Message }); "
    "[Console]::Out.Write((ConvertTo-Json -InputObject ([pscustomobject]@{ E = $er; T = $o }) -Compress))")
_TOK_CACHE = {}


def _tokens(text):
    """Real PowerShell tokens (PSParser.Tokenize) of `text`: list of (type, content, start,
    length, 1-based start line). Comments and strings are genuine tokens, so no regex can be
    fooled by `#` inside a string or `<#` inside a line comment. Skips only off Windows when
    PowerShell is absent; any tokenizer failure or parse error is a hard test failure."""
    import json, os, subprocess, tempfile
    text = text.lstrip("﻿")
    if text in _TOK_CACHE:
        return _TOK_CACHE[text]
    ps = _need_ps()
    fd, path = tempfile.mkstemp(suffix=".ps1.txt")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(text.encode("utf-8"))
        r = subprocess.run([ps, "-NoProfile", "-Command", _TOKENIZE_PS], capture_output=True,
                           text=True, encoding="utf-8", timeout=120,
                           env={**os.environ, "KILNCTL_TOK_FILE": path})
    finally:
        os.unlink(path)
    assert r.returncode == 0 and r.stdout.strip(), ("PSParser tokenizer failed", r.stderr)
    doc = json.loads(r.stdout)
    assert not doc["E"], ("PSParser reported parse errors (testnetfx9 INFO-4)", doc["E"])
    toks = [(d["T"], d["C"], d["S"], d["L"], d["N"]) for d in doc["T"]]
    _TOK_CACHE[text] = toks
    return toks


def _exec_lines(text):
    """Lines of a PowerShell file with every Comment TOKEN (line and <# #> alike) blanked
    (line numbers preserved) and quotes normalised."""
    text = text.lstrip("﻿")
    chars = list(text)
    for typ, _c, start, length, _n in _tokens(text):
        if typ == "Comment":
            for i in range(start, min(start + length, len(chars))):
                if chars[i] not in "\r\n":
                    chars[i] = " "
    return [l.strip().replace('"', "'") for l in "".join(chars).splitlines()]


_GUARD_VAR = "kilnctl_test_guard_probe"
_ENV_CMDS = {"remove-item", "ri", "rm", "del", "erase", "rd", "set-item", "si", "clear-item", "cli",
             "new-item", "ni", "rename-item", "rni", "copy-item", "move-item", "set-content",
             "add-content", "out-file", "clear-content", "set-itemproperty", "remove-itemproperty"}
_EVAL_CMDS = {"invoke-expression", "iex"}


def _env_name_hits_guard(name):
    """True if an env var written as `name` could be KILNCTL_TEST_GUARD* (fail closed on
    anything dynamic, wildcarded or unresolvable)."""
    import fnmatch
    n = name.strip("'\"").lower()
    if not n or any(c in n for c in "$()+`{}[]"):
        return True
    stem = n.split("*")[0].split("?")[0]
    return n.startswith("kilnctl_test_guard") or _GUARD_VAR.startswith(stem) or fnmatch.fnmatch(_GUARD_VAR, n)


def _env_write_problems(text, last_line=None):
    """Problems for any write to an env var that could be KILNCTL_TEST_GUARD*, by any form
    (assignment, *-Item Env:, SetEnvironmentVariable, concatenated/dynamic names) or any eval
    construct, in tokens on lines <= last_line (1-based; None = whole file)."""
    import re
    toks = [t for t in _tokens(text) if t[0] != "Comment" and (last_line is None or t[4] <= last_line)]
    probs = []
    for i, (typ, c, _s, _l, ln) in enumerate(toks):
        cl = c.lower()
        if typ == "Variable" and cl.startswith("env:"):
            nxt = toks[i + 1] if i + 1 < len(toks) else None
            if nxt and nxt[0] == "Operator" and "=" in nxt[1] and _env_name_hits_guard(c[4:]):
                probs.append((ln, "assignment to " + c))
        elif typ == "Member" and cl == "setenvironmentvariable":
            probs.append((ln, "SetEnvironmentVariable"))
        elif typ == "Command" and cl in _EVAL_CMDS:
            probs.append((ln, "eval command " + c))
        elif typ == "Command" and cl in _ENV_CMDS:
            args = []
            for t in toks[i + 1:]:
                if t[0] in ("NewLine", "StatementSeparator"):
                    break
                args.append(t[1])
            joined = " ".join(args)
            if re.search(r"env:|HKEY|Environment", joined, re.I):
                m = re.search(r"env:[\\/]?([^\s,;)]*)", joined, re.I)
                if m is None or _env_name_hits_guard(m.group(1)) or "+" in joined:
                    probs.append((ln, c + " on Env: " + joined))
    return probs


# testnetfx9 LOW-1: ALLOW-LIST, not deny-list. Everything that can execute before the probe must
# be one of the shapes below; anything else fails, so an unlisted disarm form cannot pass.
_SCAN_CMDS = {"write-error", "write-host", "join-path", "split-path", "resolve-path", "test-path",
              "get-item", "get-command", "select-object", "fail", "gate", "resolve-pctoolspython",
              "git"}
_SCAN_CALLED_FUNCS = {"fail", "gate", "resolve-pctoolspython"}
_SCAN_MEMBERS = {"supportsshouldprocess", "mandatory", "error", "writeline", "trim", "count", "length", "isnullorempty",
                 "isnullorwhitespace", "source"}
_SCAN_TYPES = {"string", "console", "switch", "int", "bool"}
_SCAN_ATTRS = {"parameter", "validateset", "cmdletbinding"}
_SCAN_KEYWORDS = {"if", "else", "elseif", "try", "catch", "finally", "return", "exit", "throw", "param",
                  "foreach", "in"}
_SCAN_ENV_VARS = {"env:kilnctl_test_no_hub", "env:kilnctl_test_guard_probe", "env:kilnctl_pctools_python"}
_SCAN_BAN_TEXT = re.compile(r"env:|environment|hkey|psdrive|psprovider|scriptblock|powershell|pwsh|"
                            r"\bcmd\b|invoke|getmethod|-enc\b|sessionstate|psvariable|executioncontext", re.I)
_CALLOP_PREV = ("NewLine", "StatementSeparator", "GroupStart", "Keyword", "Operator")


def _split_defs(toks):
    """(top, defs): top = tokens outside any function body; defs = {lower name: body tokens}.
    A function definition runs nothing by itself; its body is scanned only if it is called."""
    top, defs, i, n = [], {}, 0, len(toks)
    while i < n:
        typ, c = toks[i][0], toks[i][1]
        if typ == "Keyword" and c.lower() in ("function", "filter"):
            j = i + 1
            name = toks[j][1].lower()
            while not (toks[j][0] == "GroupStart" and toks[j][1].endswith("{")):
                j += 1
            depth = 0
            while True:
                if toks[j][0] == "GroupStart" and toks[j][1].endswith("{"):
                    depth += 1
                elif toks[j][0] == "GroupEnd" and toks[j][1] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            defs[name] = toks[i + 2:j + 1]
            i = j + 1
            continue
        top.append(toks[i])
        i += 1
    return top, defs


def _is_call_op(toks, i):
    """Operator `.` in statement-start position = dot-source / call operator (not property access)."""
    return toks[i][0] == "Operator" and toks[i][1] == "." and (i == 0 or toks[i - 1][0] in _CALLOP_PREV)


def _lib_dot_source(toks, i, script_path):
    """Resolved path if toks[i] starts the one allowed `. (Join-Path $PSScriptRoot "x.ps1")` form
    (x.ps1 in the script's own directory and existing), else None."""
    import os
    shape = toks[i + 1:i + 6]
    if len(shape) == 5 and [(s[0], s[1].lower()) for s in shape[:3]] == [
            ("GroupStart", "("), ("Command", "join-path"), ("Variable", "psscriptroot")] \
            and shape[3][0] == "String" and shape[3][1].lower().endswith(".ps1") \
            and "/" not in shape[3][1] and "\\" not in shape[3][1] and shape[4][:2] == ("GroupEnd", ")"):
        cand = os.path.join(os.path.dirname(script_path), shape[3][1])
        return cand if os.path.isfile(cand) else None
    return None


def _string_problem(c):
    if _SCAN_BAN_TEXT.search(c):
        return "banned text in " + repr(c[:60])
    for m in re.finditer(r"\$\(([^)]*)\)", c):
        if not re.fullmatch(r"\$\w+(?:\.\w+|\[\d+\])*", m.group(1).strip()):
            return "subexpression in string: " + m.group(0)
    return None


def _strict_problems(toks, allowed_dot=frozenset()):
    """Problems for any token outside the allow-list. `toks` has no Comment tokens."""
    probs = []
    for i, (typ, c, s, _l, ln) in enumerate(toks):
        cl = c.lower().strip("[]")
        prev = toks[i - 1] if i else None
        nxt = toks[i + 1] if i + 1 < len(toks) else None
        why = None
        if typ == "Command":
            if cl not in _SCAN_CMDS:
                why = "command not on allow-list: " + c
        elif typ == "Operator":
            if c == "&":
                if not (nxt and ((nxt[0] == "Variable" and nxt[1].lower() == "python")
                                 or (nxt[0] == "Command" and nxt[1].lower() == "git"))):
                    why = "call operator on something other than $python / git"
            elif c == "." and _is_call_op(toks, i):
                if s not in allowed_dot:
                    why = "dot-source / call operator not the resolved lib dot-source"
            elif c == "::":
                if not (prev and prev[0] == "Type" and prev[1].lower().strip("[]") in ("console", "string")):
                    why = "static member access on a type other than [Console]/[string]"
        elif typ == "Member":
            if cl not in _SCAN_MEMBERS:
                why = "member not on allow-list: " + c
        elif typ == "Type":
            if cl not in _SCAN_TYPES:
                why = "type not on allow-list: " + c
        elif typ == "Attribute":
            if cl not in _SCAN_ATTRS:
                why = "attribute not on allow-list: " + c
        elif typ == "Keyword":
            if cl not in _SCAN_KEYWORDS:
                why = "keyword not on allow-list: " + c
        elif typ == "Variable":
            if ":" in c:
                if cl not in _SCAN_ENV_VARS:
                    why = "provider-qualified variable: " + c
            elif cl in ("executioncontext", "host", "pshome", "myinvocation", "pscmdlet"):
                why = "variable not allowed before the probe: " + c
        elif typ in ("String", "CommandArgument", "CommandParameter"):
            why = _string_problem(c)
        elif typ in ("Unknown", "LoopLabel", "Position"):
            why = "token type not allowed: " + typ
        if why:
            probs.append((ln, why))
    return probs


def _assert_probe_is_first_statement(text, guard_line):
    """testnetfx10 LOW-1: the guard `if` is the first executable token after the param() block
    (nothing can run before it)."""
    toks = [x for x in _tokens(text) if x[0] not in ("Comment", "NewLine")]
    k = next(i for i, x in enumerate(toks) if x[0] == "Keyword" and x[1].lower() == "param")
    depth = 0
    for j in range(k + 1, len(toks)):
        if toks[j][0] == "GroupStart":
            depth += 1
        elif toks[j][0] == "GroupEnd":
            depth -= 1
            if depth == 0:
                break
    first = toks[j + 1]
    assert first[0] == "Keyword" and first[1].lower() == "if" and first[4] == guard_line,         ("first executable statement after param() must be the guard", first, guard_line)


_FIRST_STMT = {"web", "hs-start", "hs-stop", "hs-status"}


def _pre_probe_problems(text, probe_line, script_path):
    """Allow-list scan of everything that can execute from the start of `text` through
    `probe_line` (1-based), plus the bodies of the functions it calls and the one allowed
    dot-sourced lib (which must hold only function definitions)."""
    toks = [t for t in _tokens(text) if t[0] != "Comment"]
    top, defs = _split_defs(toks)
    allowed_dot, probs = set(), []
    for i, t in enumerate(top):
        if _is_call_op(top, i) and t[4] <= probe_line:
            lib = _lib_dot_source(top, i, script_path)
            if lib is None:
                probs.append((t[4], "dot-source / call operator that is not the resolved lib dot-source"))
                continue
            allowed_dot.add(t[2])
            with open(lib, encoding="utf-8") as fh:
                ltop, ldefs = _split_defs([x for x in _tokens(fh.read()) if x[0] != "Comment"])
            if [x for x in ltop if x[0] not in ("NewLine", "StatementSeparator")]:
                probs.append((t[4], "dot-sourced lib has top-level code: " + lib))
            defs.update(ldefs)
    pre = [t for t in top if t[4] <= probe_line]
    probs += _strict_problems(pre, frozenset(allowed_dot))
    for name in sorted({t[1].lower() for t in pre if t[0] == "Command"} & set(defs)):
        if name not in _SCAN_CALLED_FUNCS:
            probs.append((0, "user function called before the probe: " + name))
        else:
            probs += _strict_problems(defs[name])
    return probs


def test_nofetch_env_is_network_dead():
    """testnetfx7 MED-1: under _NOFETCH_ENV git resolves origin to a local file URL (never
    github.com) and only the file protocol is allowed. ls-remote --get-url is purely local."""
    import os, subprocess
    protos = _NOFETCH_ENV["GIT_ALLOW_PROTOCOL"].split(":")
    assert protos == ["file"] and not {"https", "http", "ssh", "git"} & set(protos)
    env = {**os.environ, **_NOFETCH_ENV}
    r = subprocess.run(["git", "-C", _repo_file(), "ls-remote", "--get-url", "origin"],
                       capture_output=True, text=True, env=env, timeout=30)
    assert r.returncode == 0, r.stderr
    assert "github.com" not in r.stdout and r.stdout.strip().startswith("file://"), r.stdout


# Controls never reach real network even if a probe is dead: -SkipBuild, and git is made
# network-dead (testnetfx7 MED-1): remote.origin.url is multi-valued so a command-line value
# is APPENDED; an empty value first resets the list, and GIT_ALLOW_PROTOCOL=file refuses every
# non-file transport regardless. The web control's credentials are protected by the script's
# own CRED-BACKSTOP (the script overwrites env credentials, so dummies would be dead config).
_NOFETCH_ENV = {"GIT_CONFIG_COUNT": "2",
                "GIT_CONFIG_KEY_0": "remote.origin.url", "GIT_CONFIG_VALUE_0": "",
                "GIT_CONFIG_KEY_1": "remote.origin.url",
                "GIT_CONFIG_VALUE_1": "file:///nonexistent/kilnctl-test-no-fetch",
                "GIT_ALLOW_PROTOCOL": "file",
                "GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "never"}
_HS = ("tools", "PcTools", "scripts", "bench_hotspot.ps1")
_WEB = ("tools", "PcTools", "scripts", "web_commission_row.ps1")
_MR = ("tools", "make_release.ps1")
_PROBE_RC = 86
_HS_ANCHOR = "if ($env:KILNCTL_TEST_GUARD_PROBE) {"  # the exit-87 backstop line
_NH = "KILNCTL_TEST_NO_HUB"
# id, script, args, refusal needle, guard line (single-quoted form), first executable line
# after the probe, marker the probe prints, extra env
_CASES = [
    ("web", _WEB, ["-Row", "W28", "-BoardHost", "127.0.0.1:1", "-ScreenshotDir", "unused"],
     "live mode refused", "if (-not $DryRun -and $env:" + _NH + " -eq '1') {",
     "$ErrorActionPreference = 'Stop'", _PROBE, {}),
    ("hs-start", _HS, ["-Action", "start"], "start refused", "if ($env:" + _NH + " -eq '1') {",
     _HS_ANCHOR, _PROBE, {}),
    ("hs-stop", _HS, ["-Action", "stop"], "stop refused", "if ($env:" + _NH + " -eq '1') {",
     _HS_ANCHOR, _PROBE, {}),
    ("hs-status", _HS, ["-Action", "status"], "status refused", "if ($env:" + _NH + " -eq '1') {",
     _HS_ANCHOR, _PROBE, {}),
    ("publish", _MR, ["-Tag", "v0.0.0-test.1", "-Publish", "-SkipBuild"], "-Publish refused",
     "if ($Publish -and $env:" + _NH + " -eq '1') {", "function Get-ReleaseApiHeaders",
     _PROBE + " publish", _NOFETCH_ENV),
    ("devdryrun", _MR, ["-Tag", "v0.0.0-test.1", "-DevDryRun", "-SkipBuild"], "network git access refused",
     "if ($env:" + _NH + " -eq '1') {", "& git -C $repoRoot fetch --quiet origin release --tags",
     _PROBE, _NOFETCH_ENV),
]
_IDS = [c[0] for c in _CASES]
_PARAMS = "cid,script_parts,args,needle,guard,anchor,marker,xenv"


@pytest.mark.parametrize(_PARAMS, _CASES, ids=_IDS)
def test_user_scope_credential_scripts_refuse_under_test_env(cid, script_parts, args, needle, guard, anchor, marker, xenv):
    """Testnet3 L3 / testnet4 L2+L3: these scripts use User-scope credentials or the real
    hotspot / git credential manager, which the conftest env drop cannot hide. Under
    KILNCTL_TEST_NO_HUB=1 each must refuse with that guard's OWN message before the action.

    Safe on failure: KILNCTL_TEST_GUARD_PROBE=1 makes each script write the probe marker to
    stderr and exit 86 just PAST its guard, so a regressed guard cannot start the hotspot,
    publish, fetch or load credentials. (No sentinel-secret assertion: the scripts read the
    User-scope registry, never the process env, so it would be vacuous.)"""
    import subprocess, os
    ps = _need_ps()
    env = {**os.environ, **xenv, "KILNCTL_TEST_NO_HUB": "1", "KILNCTL_TEST_GUARD_PROBE": "1"}
    r = subprocess.run([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        _repo_file(*script_parts), *args],
                       capture_output=True, text=True, env=env, timeout=120)
    out = r.stdout + r.stderr
    assert _PROBE not in out, "guard regressed (stopped only by the probe): " + out
    assert r.returncode not in (0, _PROBE_RC), out
    assert needle in out and _NH in out, out


@pytest.mark.parametrize(_PARAMS, _CASES, ids=_IDS)
def test_guard_probe_reaches_past_guard_without_no_hub(cid, script_parts, args, needle, guard, anchor, marker, xenv):
    """Control for the refusal test: with the guard condition off, each invocation reaches ITS
    OWN probe (site marker on stderr, exit 86, never 0). Inputs are inert even if a probe is
    dead: closed localhost port, no credentials (the web script's backstop stops it), -SkipBuild, unreachable origin."""
    import subprocess, os
    ps = _need_ps()
    env = {k: v for k, v in os.environ.items() if k != _NH}
    env.update(xenv)
    env["KILNCTL_TEST_GUARD_PROBE"] = "1"
    r = subprocess.run([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        _repo_file(*script_parts), *args], capture_output=True, text=True,
                       env=env, timeout=120)
    assert _BACKSTOP not in r.stderr, r.stdout + r.stderr
    assert r.returncode == _PROBE_RC and marker in r.stderr.strip().splitlines(), r.stdout + r.stderr


@pytest.mark.parametrize(_PARAMS, _CASES, ids=_IDS)
def test_guard_probe_precedes_real_action(cid, script_parts, args, needle, guard, anchor, marker, xenv):
    """Static pin: an executable guard line, then an executable probe `if (` whose condition is
    the guard's with NO_HUB -> GUARD_PROBE (so not commented, no `$false -and` prefix), then
    only blanks/comments, then the executable anchor line. The probe writes the marker to
    stderr and exits 86."""
    import re
    with open(_repo_file(*script_parts), encoding="utf-8") as fh:
        text = fh.read()
    lines = text.splitlines()
    norm = _exec_lines(text)
    gs = [n for n, l in enumerate(norm) if l == guard]
    assert len(gs) == 1, ("guard line not found exactly once", guard, gs)
    want = guard.replace(_NH, "KILNCTL_TEST_GUARD_PROBE")
    probes = [n for n, l in enumerate(norm) if l.startswith(want) and _PROBE in l]
    assert len(probes) == 1, ("expected exactly one executable probe `if` on the guard's condition", want,
                              [lines[n] for n, l in enumerate(lines) if "KILNCTL_TEST_GUARD_PROBE" in l])
    pn = probes[0]
    assert "[Console]::Error.WriteLine('" + marker + "')" in norm[pn] and ("exit %d" % _PROBE_RC) in norm[pn], lines[pn]
    assert gs[0] < pn, ("guard must precede probe", gs[0], pn)
    # testnetfx7 LOW-1: guard..probe holds only the refusal body and its closing brace; no
    # disarm of the probe variable and no network command in that executable span.
    # testnetfx8 LOW-1: token-based; no disarm of KILNCTL_TEST_GUARD* by ANY form before the
    # probe, including in files dot-sourced before it.
    probs = _env_write_problems(text, pn + 1)
    assert not probs, ("env write before the probe", probs)
    # testnetfx10 LOW-1: allow-list, not deny-list: only the listed token shapes may execute
    # before the probe (commands, members, types, call operators, the one lib dot-source).
    probs = _pre_probe_problems(text, pn + 1, _repo_file(*script_parts))
    assert not probs, ("token outside the pre-probe allow-list", probs)
    if cid in _FIRST_STMT:
        _assert_probe_is_first_statement(text, gs[0] + 1)
    span = [(n, norm[n]) for n in range(gs[0] + 1, pn) if norm[n] and not norm[n].startswith("#")]
    assert span and span[-1][1] == "}", ("guard block must close with } right before the probe", span)
    bad = re.compile(r"GUARD_PROBE|Remove-Item|Set-Item|Clear-Item|\bgit\b|ls-remote|\bfetch\b|\bpush\b|"
                     r"Invoke-WebRequest|Invoke-RestMethod|\biwr\b|\birm\b|\bcurl\b|\bgh\b|Start-Process|"
                     r"WebClient|Start-Job|SetEnvironmentVariable|\bEnv:", re.I)
    for n, l in span:
        assert not bad.search(re.sub(r"'[^']*'", "''", l)), ("disarm or network command between guard and probe", lines[n])
    for n, l in enumerate(norm):
        if n != pn and not l.startswith("#") and re.search(r"GUARD_PROBE", l) and "-eq" not in l \
                and "-not" not in l and "$env:" + "KILNCTL_TEST_GUARD_PROBE)" not in l:
            assert False, ("probe variable assigned/cleared elsewhere", lines[n])
    later = [n for n, l in enumerate(norm) if n > pn and l.startswith(anchor)]
    assert later, ("executable anchor not found after the probe", anchor)
    for n in range(pn + 1, later[0]):
        assert norm[n] == "" or norm[n].startswith("#"), ("executable line between probe and anchor", lines[n])


@pytest.mark.parametrize("script_parts,actions", [
    (_WEB, ["GetEnvironmentVariable"]),
    (_HS, ["GetEnvironmentVariable", "switch ($Action)", "TetheringManager"]),
], ids=["web", "hotspot"])
def test_credential_backstop_precedes_credential_load(script_parts, actions):
    """testnetfx7 LOW-3 / testnetfx8 LOW-2: the script refuses (exit 87) while the test probe
    variable is set, as one executable line AFTER the probe and BEFORE the first credential
    read, tethering call or action dispatch."""
    with open(_repo_file(*script_parts), encoding="utf-8") as fh:
        norm = _exec_lines(fh.read())
    want = ("if ($env:KILNCTL_TEST_GUARD_PROBE) { [Console]::Error.WriteLine('" + _BACKSTOP + "'); exit 87 }")
    bs = [n for n, l in enumerate(norm) if l == want]
    assert len(bs) == 1, ("backstop must appear exactly once as an executable line", bs)
    probe = [n for n, l in enumerate(norm) if _PROBE in l and "exit 86" in l]
    assert probe and probe[-1] < bs[0], ("backstop must follow the probe", probe, bs)
    acts = [n for n, l in enumerate(norm) if any(a in l for a in actions) and n > probe[-1]]
    assert acts and bs[0] < min(acts), (bs, acts)
    for n in range(bs[0] + 1, min(acts)):
        assert norm[n] == "", norm[n]


def test_settings_override_prints_notice():
    """Testnet5 L4: when the override is active, importing settings says so on stderr."""
    import subprocess, sys, os
    here = os.path.dirname(__file__)
    env = {**os.environ, "KILNCTL_TEST_SETTINGS_PATH": os.path.join(str(here), "notice_probe.json"),
           "PYTHONPATH": os.path.join(here, "..", "src")}
    r = subprocess.run([sys.executable, "-c", "import kilnctrl.settings"], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert "KILNCTL_TEST_SETTINGS_PATH" in r.stderr and "notice_probe.json" in r.stderr, r.stderr
    env.pop("KILNCTL_TEST_SETTINGS_PATH")
    r = subprocess.run([sys.executable, "-c", "import kilnctrl.settings"], capture_output=True, text=True, env=env)
    assert "KILNCTL_TEST_SETTINGS_PATH" not in r.stderr, r.stderr


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


@pytest.fixture
def _own_refused_port(monkeypatch):
    """INFO-1: a loopback TCP listener this test binds itself, its port registered as refused
    (an MCP-server port). If a guard regresses the connection lands on OUR listener only."""
    import conftest
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(5)
    port = srv.getsockname()[1]
    assert port not in (8765, 8766, 8767)
    monkeypatch.setattr(conftest, "_MCP_SERVER_PORTS", (port,))
    yield srv, port
    srv.close()


def test_guard_refuses_asyncio_connections(_own_refused_port, _no_outbound_network):
    """oldpoolb MED-1: asyncio (ProactorEventLoop on Windows) bypasses socket.socket.connect.
    The target is a loopback listener the test bound itself and registered as refused, so a
    regressed guard connects only to that listener."""
    import asyncio
    srv, port = _own_refused_port

    async def go():
        await asyncio.open_connection("127.0.0.1", port)

    with pytest.raises(NetworkGuardError):
        asyncio.run(go())
    assert len(_no_outbound_network) == 1, _no_outbound_network
    _no_outbound_network.clear()


def test_guard_refuses_selector_loop_connections(_own_refused_port, _no_outbound_network):
    """oldpoolb MED-1: the selector loop's sock_connect is guarded too (own refused port)."""
    import asyncio
    srv, port = _own_refused_port
    loop = asyncio.SelectorEventLoop()
    try:
        with pytest.raises(NetworkGuardError):
            loop.run_until_complete(loop.create_connection(asyncio.Protocol, "127.0.0.1", port))
    finally:
        loop.close()
    assert len(_no_outbound_network) == 1, _no_outbound_network
    _no_outbound_network.clear()


def test_guard_refuses_asyncio_udp_sendto(monkeypatch, _no_outbound_network):
    """testnetfx7 LOW-2: Proactor UDP sendto (loop.sock_sendto -> IocpProactor.sendto) is
    guarded. The target is a UDP socket this test bound itself, registered as refused."""
    import asyncio, conftest
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    rx.setblocking(False)
    port = rx.getsockname()[1]
    assert port not in (8765, 8766, 8767)
    monkeypatch.setattr(conftest, "_MCP_SERVER_PORTS", (port,))

    async def go():
        loop = asyncio.get_running_loop()
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tx.bind(("127.0.0.1", 0))
        tx.setblocking(False)
        try:
            with pytest.raises(NetworkGuardError):
                await loop.sock_sendto(tx, b"x", ("127.0.0.1", port))
        finally:
            tx.close()

    try:
        asyncio.run(go())
        assert len(_no_outbound_network) == 1, _no_outbound_network
        _no_outbound_network.clear()
        with pytest.raises(BlockingIOError):
            rx.recv(10)
    finally:
        rx.close()
