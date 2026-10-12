"""Windows Job object that kills build process trees when this process dies.

REVIEW_BUILDFX3 MED-2: the build lock records the MCP server's PID, but the
build (powershell, idf.py, ninja, compilers) is a child tree that nothing
killed when the server was restarted mid-build. The orphan kept writing the
build directory while the dead-PID reclaim let a second build start beside it.

One process-wide job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: every build
child is assigned to it, and Windows closes the job handle (killing the whole
tree) when this process exits for any reason, including TerminateProcess.
Best effort: failure to create or assign returns False and the caller carries
on (the dead-PID reclaim then has its old, weaker behaviour).
"""
from __future__ import annotations

import os
import threading

_lock = threading.Lock()
_job = None  # HANDLE, kept for the life of the process on purpose

_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
_JobObjectExtendedLimitInformation = 9


def _create_job():
    import ctypes
    from ctypes import wintypes

    class BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                    ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class IOC(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in
                    ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                     "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class EXT(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BASIC), ("IoInfo", IOC),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = wintypes.HANDLE
    k32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
    k32.SetInformationJobObject.restype = wintypes.BOOL
    k32.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int,
                                            ctypes.c_void_p, wintypes.DWORD)
    k32.CloseHandle.argtypes = (wintypes.HANDLE,)
    job = k32.CreateJobObjectW(None, None)
    if not job:
        return None
    info = EXT()
    info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not k32.SetInformationJobObject(job, _JobObjectExtendedLimitInformation,
                                       ctypes.byref(info), ctypes.sizeof(info)):
        k32.CloseHandle(job)
        return None
    return job


def assign_to_kill_on_close_job(pid: int) -> bool:
    """Put ``pid`` (and so its future descendants) in the kill-on-close job."""
    global _job
    if os.name != "nt":
        return False
    try:
        import ctypes
        from ctypes import wintypes
        with _lock:
            if _job is None:
                _job = _create_job()
            job = _job
        if not job:
            return False
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.OpenProcess.restype = wintypes.HANDLE
        k32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        k32.AssignProcessToJobObject.restype = wintypes.BOOL
        k32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        k32.CloseHandle.argtypes = (wintypes.HANDLE,)
        # PROCESS_SET_QUOTA | PROCESS_TERMINATE
        h = k32.OpenProcess(0x0100 | 0x0001, False, pid)
        if not h:
            return False
        try:
            return bool(k32.AssignProcessToJobObject(job, h))
        finally:
            k32.CloseHandle(h)
    except Exception:  # pragma: no cover - best effort
        return False
