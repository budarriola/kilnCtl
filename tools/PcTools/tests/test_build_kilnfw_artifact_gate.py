"""build_kilnfw must judge success by exit code AND a usable, fresh app image,
and every idf.py child must run without Git Bash's MSYSTEM (2026-10-11: two
false OKs; 2026-10-12 review fixes: incremental builds, non-build targets).
A fake idf.py (a python script) stands in for the toolchain."""
from __future__ import annotations

import contextlib
import os
import shutil
import sys
import time

import pytest

from _kilnfw_fakes import (make_app_image, write_app_bin, write_fresh_bins,
                           write_static_bins)
from mcpkit import workbench

FAKE_IDF = r"""
import os, sys
mode = sys.argv[1]
build = sys.argv[2]
if mode == "msys":
    print("MSys/Mingw is no longer supported"); sys.exit(0)
if mode == "nobin":
    print("done"); sys.exit(0)
if mode == "bins":
    print("built"); sys.exit(0)
if mode == "env":
    print("MSYSTEM=" + repr(os.environ.get("MSYSTEM"))); sys.exit(0)
"""


@pytest.fixture
def harness(monkeypatch, tmp_path):
    fw = tmp_path / "firmware" / "KilnFW"
    (tmp_path / "firmware" / "SaftyFW").mkdir(parents=True)
    fw.mkdir()
    (fw / "CMakeLists.txt").write_text("")
    (tmp_path / "firmware" / "SaftyFW" / "CMakeLists.txt").write_text("")
    (fw / "sdkconfig").write_text("")
    script = tmp_path / "fake_idf.py"
    script.write_text(FAKE_IDF)
    build = str(fw / "build")
    os.makedirs(build)
    state = {"mode": "nobin", "after": None, "calls": []}

    def fake_run_locked(tag, key, argv, **kwargs):
        with contextlib.nullcontext():
            lock_info = kwargs.get("lock_info")
            if lock_info is not None:
                lock_info["start_wall"] = time.time()
            state["calls"].append((tag, kwargs))
            out = workbench._run(tag, [sys.executable, str(script), state["mode"], build],
                                 env=kwargs.get("env"),
                                 output_check=kwargs.get("output_check"))
            if state["after"]:
                state["after"](build)
            return out

    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)
    return fw, build, state


def _build(fw, target="build"):
    return workbench.build_kilnfw(target=target, skip_saftyfw=True, kiln_fw_root=str(fw))


def test_exit0_but_no_bin_is_failed(harness):
    fw, build, state = harness
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "missing" in out
    assert "full log:" in out


def test_msys_message_is_failed(harness):
    fw, build, state = harness
    state["mode"] = "msys"
    state["after"] = write_fresh_bins
    out = _build(fw)
    assert "FAILED" in out and "MSys/Mingw" in out
    assert "full log:" in out


def test_fresh_bins_are_ok(harness):
    fw, build, state = harness
    state["mode"] = "bins"
    state["after"] = write_fresh_bins
    assert "kilnfw-build: OK" in _build(fw)


def test_incremental_build_old_bootloader_and_table_is_ok(harness):
    """HIGH-1: ninja relinks KilnCtrl.bin but leaves bootloader/partition table
    a month old."""
    fw, build, state = harness
    state["mode"] = "bins"

    def incremental(b):
        write_app_bin(b)
        write_static_bins(b, age_s=30 * 86400)

    state["after"] = incremental
    out = _build(fw)
    assert "kilnfw-build: OK" in out, out


def test_stale_app_bin_is_failed(harness):
    """KilnCtrl.bin from an earlier build (both its embedded time and mtime old)
    with an exit-0 no-op build."""
    fw, build, state = harness
    old = time.time() - 3600
    write_app_bin(build, built_at=old, mtime=old)
    write_static_bins(build)
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "stale" in out and "KilnCtrl.bin" in out


def test_stale_app_bin_tolerance_is_two_seconds(harness):
    """Aged ~10 s before the job start: must fail. Pins the 2 s tolerance."""
    fw, build, state = harness
    old = time.time() - 10
    write_app_bin(build, built_at=old, mtime=old)
    write_static_bins(build)
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "stale" in out


def test_app_bin_within_tolerance_is_ok(harness):
    fw, build, state = harness
    state["after"] = lambda b: write_app_bin(b, built_at=time.time() - 1, mtime=time.time() - 1)
    write_static_bins(build)
    assert "kilnfw-build: OK" in _build(fw)


@pytest.mark.parametrize("victim", ["KilnCtrl.bin",
                                    os.path.join("bootloader", "bootloader.bin"),
                                    os.path.join("partition_table", "partition-table.bin")])
def test_each_missing_artifact_is_failed(harness, victim):
    fw, build, state = harness

    def all_but_one(b):
        write_fresh_bins(b)
        os.remove(os.path.join(b, victim))

    state["after"] = all_but_one
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "missing" in out
    assert victim in out


@pytest.mark.parametrize("victim", ["KilnCtrl.bin",
                                    os.path.join("bootloader", "bootloader.bin"),
                                    os.path.join("partition_table", "partition-table.bin")])
def test_zero_byte_artifact_is_failed(harness, victim):
    fw, build, state = harness

    def one_empty(b):
        write_fresh_bins(b)
        open(os.path.join(b, victim), "wb").close()

    state["after"] = one_empty
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "empty" in out and victim in out


def test_app_bin_without_app_desc_magic_is_failed(harness):
    fw, build, state = harness

    def bad_magic(b):
        write_fresh_bins(b)
        with open(os.path.join(b, "KilnCtrl.bin"), "wb") as fh:
            fh.write(make_app_image(magic=0x12345678))

    state["after"] = bad_magic
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "not a valid app image" in out


def test_job_start_is_taken_after_lock_acquired(harness, monkeypatch):
    """LOW-1: bins written by another holder while we waited for the lock are
    older than the post-lock baseline, so a no-op exit-0 build fails."""
    fw, build, state = harness
    write_static_bins(build)
    write_app_bin(build)  # fresh relative to the call, but written before the lock

    real = workbench._run_locked

    def lock_waits(tag, key, argv, **kwargs):
        # simulate a long lock wait: the baseline lands 60 s after the bins
        lock_info = kwargs.get("lock_info")
        out = real(tag, key, argv, **kwargs)
        if lock_info is not None:
            lock_info["start_wall"] = time.time() + 60
        return out

    monkeypatch.setattr(workbench, "_run_locked", lock_waits)
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "stale" in out


# -- MED-1: non-build targets -------------------------------------------------

def test_reconfigure_with_stale_cmakecache_is_failed(harness):
    fw, build, state = harness
    cache = os.path.join(build, "CMakeCache.txt")
    open(cache, "w").close()
    old = time.time() - 3600
    os.utime(cache, (old, old))
    out = _build(fw, target="reconfigure")
    assert "kilnfw-reconfigure: FAILED" in out and "CMakeCache.txt" in out


def test_reconfigure_with_fresh_cmakecache_is_ok(harness):
    fw, build, state = harness
    state["after"] = lambda b: open(os.path.join(b, "CMakeCache.txt"), "w").close()
    assert "kilnfw-reconfigure: OK" in _build(fw, target="reconfigure")


def test_reconfigure_with_no_cmakecache_is_failed(harness):
    fw, build, state = harness
    out = _build(fw, target="reconfigure")
    assert "kilnfw-reconfigure: FAILED" in out and "missing" in out


def test_fullclean_leaving_files_is_failed(harness):
    fw, build, state = harness
    open(os.path.join(build, "CMakeCache.txt"), "w").close()
    out = _build(fw, target="fullclean")
    assert "kilnfw-fullclean: FAILED" in out and "not empty" in out


def test_fullclean_empty_or_absent_build_dir_is_ok(harness):
    fw, build, state = harness
    assert "kilnfw-fullclean: OK" in _build(fw, target="fullclean")
    state["after"] = lambda b: shutil.rmtree(b, ignore_errors=True)
    open(os.path.join(build, "x"), "w").close()
    assert "kilnfw-fullclean: OK" in _build(fw, target="fullclean")


def test_msys_message_on_non_build_target_is_failed(harness):
    fw, build, state = harness
    state["mode"] = "msys"
    out = _build(fw, target="fullclean")
    assert "FAILED" in out and "MSys/Mingw" in out
    assert "full log:" in out


def test_msys_check_is_an_output_check_on_every_kilnfw_step(harness):
    fw, build, state = harness
    state["mode"] = "bins"
    state["after"] = write_fresh_bins
    _build(fw)
    assert state["calls"]
    for tag, kwargs in state["calls"]:
        assert kwargs.get("output_check") is workbench._msys_output_problems, tag


def test_set_target_and_submodule_steps_get_msys_output_check(monkeypatch, tmp_path):
    fw = tmp_path / "firmware" / "KilnFW"
    (tmp_path / "firmware" / "SaftyFW").mkdir(parents=True)
    fw.mkdir()
    (fw / "CMakeLists.txt").write_text("")
    (tmp_path / "firmware" / "SaftyFW" / "CMakeLists.txt").write_text("")
    seen = {}

    def fake(tag, key, argv, **kwargs):
        seen[tag] = kwargs.get("output_check")
        return f"{tag}: OK in 1s (1 log lines)\nfull log: y\n--\nok"

    monkeypatch.setattr(workbench, "_run_locked", fake)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)
    workbench.build_kilnfw(target="reconfigure", skip_saftyfw=True, kiln_fw_root=str(fw))
    assert seen["kilnfw-submodule-init"] is workbench._msys_output_problems
    assert seen["kilnfw-set-target"] is workbench._msys_output_problems
    assert seen["kilnfw-reconfigure"] is workbench._msys_output_problems


# -- wrapper text -------------------------------------------------------------

def test_wrapper_fails_closed():
    cmd = workbench._idf_command("idf.py -C 'x' build")
    assert "$ErrorActionPreference='Stop'" in cmd
    assert "Get-Command idf.py" in cmd and "exit 97" in cmd
    assert "$null -eq $LASTEXITCODE" in cmd and "exit 98" in cmd
    assert cmd.rstrip().endswith("exit $LASTEXITCODE")
    assert "Get-Command ninja" in workbench._idf_command("ninja", "ninja")


# -- env strip, each point on its own ----------------------------------------

def _probe_argv():
    return [sys.executable, "-c",
            "import os;print('MSYSTEM=%r'%os.environ.get('MSYSTEM'),"
            "'PFX=%r'%os.environ.get('MSYSTEM_PREFIX'))"]


@pytest.fixture
def msys_env(monkeypatch):
    monkeypatch.setenv("MSYSTEM", "MINGW64")
    monkeypatch.setenv("MSYSTEM_PREFIX", "/mingw64")
    monkeypatch.setattr(workbench, "kiln_build_gate", lambda *a, **k: contextlib.nullcontext())
    monkeypatch.setattr(workbench, "build_lock", lambda *a, **k: contextlib.nullcontext())


def test_run_strips_msystem(msys_env):
    """_run on its own (the ungated callers: set-target, submodule-init,
    saftyfw-configure)."""
    out = workbench._run("envprobe", _probe_argv())
    assert "MSYSTEM=None" in out and "PFX=None" in out


def test_run_locked_ungated_strips_msystem(msys_env):
    out = workbench._run_locked("envprobe", "k", _probe_argv())
    assert "MSYSTEM=None" in out and "PFX=None" in out


def test_run_locked_gated_strip_alone(msys_env, monkeypatch):
    """The gated-branch strip, with _run's own strip neutralised."""
    real_run = workbench._run
    monkeypatch.setattr(workbench, "_clean_env", workbench._clean_env)
    seen = {}

    def spy(tag, argv, **kwargs):
        seen["env"] = kwargs.get("env")
        return "ok"

    monkeypatch.setattr(workbench, "_run", spy)
    workbench._run_locked("envprobe", "k", _probe_argv(), gate_label="envprobe")
    assert "MSYSTEM" not in seen["env"] and "MSYSTEM_PREFIX" not in seen["env"]
    monkeypatch.setattr(workbench, "_run", real_run)


def test_run_strip_alone_with_gated_branch_unstripped(msys_env, monkeypatch):
    """_run's strip must hold even if a caller passes a dirty env explicitly."""
    out = workbench._run("envprobe", _probe_argv(),
                         env={**os.environ, "MSYSTEM": "UCRT64", "MSYSTEM_PREFIX": "/x"})
    assert "MSYSTEM=None" in out and "PFX=None" in out


def test_saftyfw_configure_env_has_no_msystem(monkeypatch, tmp_path, msys_env):
    seen = {}

    def fake(tag, key, argv, **kwargs):
        seen[tag] = kwargs.get("env")
        return f"{tag}: OK in 1s (1 log lines)\nfull log: y\n--\nok"

    monkeypatch.setattr(workbench, "_run_locked", fake)
    import mcpkit.pico_sdk as pico
    monkeypatch.setattr(pico, "resolve_pico_sdk_path", lambda: "C:/sdk")
    (tmp_path / "SaftyFW").mkdir()
    (tmp_path / "SaftyFW" / "CMakeLists.txt").write_text("")
    workbench.build_saftyfw(saftyfw_root=str(tmp_path / "SaftyFW"))
    env = seen["saftyfw-configure"]
    assert "MSYSTEM" not in env and env["PICO_SDK_PATH"] == "C:/sdk"


def test_incremental_old_desc_time_fresh_mtime_is_ok(harness):
    """Real incremental builds keep an old esp_app_desc time (its TU is not
    recompiled) while the link rewrites KilnCtrl.bin: freshness is the mtime."""
    fw, build, state = harness
    state["mode"] = "bins"

    def incremental(b):
        write_app_bin(b, built_at=time.time() - 86400)  # fresh mtime, old desc
        write_static_bins(b, age_s=86400)

    state["after"] = incremental
    assert "kilnfw-build: OK" in _build(fw)


# -- review BUILDFX2 LOW-1: guards that must bite -----------------------------

def test_clean_env_strip_is_case_insensitive():
    env = workbench._clean_env({"msystem": "MINGW64", "Msystem_Prefix": "/x", "KEEP": "1"})
    assert "msystem" not in env and "Msystem_Prefix" not in env and env["KEEP"] == "1"


def test_real_run_locked_records_start_wall_after_lock_acquired(monkeypatch):
    """Drives the REAL _run_locked (fake lock + fake _run) and checks the
    baseline is taken once the lock is held, not before the wait."""
    seen = {}

    @contextlib.contextmanager
    def slow_lock(key, **kw):
        time.sleep(0.2)
        seen["acquired"] = time.time()
        yield

    monkeypatch.setattr(workbench, "build_lock", slow_lock)
    monkeypatch.setattr(workbench, "_run", lambda tag, argv, **kw: f"{tag}: OK")
    info = {}
    t0 = time.time()
    workbench._run_locked("t", "k", ["x"], lock_info=info)
    assert info["start_wall"] >= seen["acquired"] >= t0 + 0.2


def test_wrapper_resets_lastexitcode_before_inner_command():
    cmd = workbench._idf_command("idf.py build")
    assert "$global:LASTEXITCODE=$null;" in cmd
    assert cmd.index("$global:LASTEXITCODE=$null") < cmd.index("idf.py build;")


def test_downgrade_head_carries_full_log_line():
    out = workbench._downgrade("t: OK\nfull log: C:/x.log\nmore", "t", "why", ["p"])
    assert "full log: C:/x.log" in out.split("\n--\n")[0]
