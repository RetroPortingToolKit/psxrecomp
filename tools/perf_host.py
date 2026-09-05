#!/usr/bin/env python3
"""Launch a benchmark child under a process-scoped host CPU budget.

Windows uses a Job Object hard CPU-rate cap. The cap is applied before the child
thread is resumed, so the target does not do meaningful work outside the budget.
Other platforms may run unrestricted, but CPU throttling fails explicitly.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import json
import math
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


class PerfHostError(RuntimeError):
    pass


class UnsupportedThrottle(PerfHostError):
    pass


@dataclass
class LaunchResult:
    pid: int
    exit_code: int | None
    timed_out: bool
    wall_seconds: float
    cpu_user_seconds: float | None
    cpu_kernel_seconds: float | None
    cpu_total_seconds: float | None


def parse_affinity(text: str | None) -> int | None:
    if text is None or text == "":
        return None
    text = text.strip()
    if not text:
        return None
    if "," in text:
        mask = 0
        for part in text.split(","):
            part = part.strip()
            if not part:
                raise ValueError("empty CPU index in affinity list")
            idx = int(part, 10)
            if idx < 0:
                raise ValueError("negative CPU index in affinity list")
            mask |= 1 << idx
        if mask == 0:
            raise ValueError("affinity mask may not be zero")
        return mask
    mask = int(text, 0)
    if mask <= 0:
        raise ValueError("affinity mask may not be zero")
    return mask


def validate_cpu_percent(value: int | None) -> int | None:
    if value is None:
        return None
    if value < 1 or value > 100:
        raise ValueError("--cpu-percent must be in 1..100")
    return value


def validate_core_percent(value: float | None) -> float | None:
    if value is None:
        return None
    if not math.isfinite(value) or value <= 0.0 or value > 100.0:
        raise ValueError("--core-percent must be finite and in (0, 100]")
    return value


def topology_report() -> dict[str, Any]:
    affinity = None
    if hasattr(os, "sched_getaffinity"):
        try:
            affinity = sorted(os.sched_getaffinity(0))  # type: ignore[attr-defined]
        except OSError:
            affinity = None
    return {
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "machine": platform.machine(),
        "logical_cpu_count": os.cpu_count(),
        "process_affinity": affinity,
    }


def _filetime_seconds(ft: Any) -> float:
    ticks = (int(ft.dwHighDateTime) << 32) | int(ft.dwLowDateTime)
    return ticks / 10_000_000.0


if os.name == "nt":
    wintypes = ctypes.wintypes

    CREATE_SUSPENDED = 0x00000004
    CREATE_NO_WINDOW = 0x08000000
    STARTF_USESHOWWINDOW = 0x00000001
    STARTF_USESTDHANDLES = 0x00000100
    SW_HIDE = 0
    INFINITE = 0xFFFFFFFF
    TERMINATE_WAIT_MS = 5000
    WAIT_OBJECT_0 = 0
    WAIT_TIMEOUT = 0x00000102
    JobObjectExtendedLimitInformation = 9
    JobObjectCpuRateControlInformation = 15
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    JOB_OBJECT_CPU_RATE_CONTROL_ENABLE = 0x1
    JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP = 0x4

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("lpReserved", wintypes.LPWSTR),
            ("lpDesktop", wintypes.LPWSTR),
            ("lpTitle", wintypes.LPWSTR),
            ("dwX", wintypes.DWORD),
            ("dwY", wintypes.DWORD),
            ("dwXSize", wintypes.DWORD),
            ("dwYSize", wintypes.DWORD),
            ("dwXCountChars", wintypes.DWORD),
            ("dwYCountChars", wintypes.DWORD),
            ("dwFillAttribute", wintypes.DWORD),
            ("dwFlags", wintypes.DWORD),
            ("wShowWindow", wintypes.WORD),
            ("cbReserved2", wintypes.WORD),
            ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
            ("hStdInput", wintypes.HANDLE),
            ("hStdOutput", wintypes.HANDLE),
            ("hStdError", wintypes.HANDLE),
        ]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("hProcess", wintypes.HANDLE),
            ("hThread", wintypes.HANDLE),
            ("dwProcessId", wintypes.DWORD),
            ("dwThreadId", wintypes.DWORD),
        ]

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_longlong),
            ("PerJobUserTimeLimit", ctypes.c_longlong),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_ulonglong),
            ("WriteOperationCount", ctypes.c_ulonglong),
            ("OtherOperationCount", ctypes.c_ulonglong),
            ("ReadTransferCount", ctypes.c_ulonglong),
            ("WriteTransferCount", ctypes.c_ulonglong),
            ("OtherTransferCount", ctypes.c_ulonglong),
        ]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
            ("IoInfo", IO_COUNTERS),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    class JOBOBJECT_CPU_RATE_CONTROL_INFORMATION(ctypes.Structure):
        _fields_ = [("ControlFlags", wintypes.DWORD), ("CpuRate", wintypes.DWORD)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateProcessW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.LPWSTR,
        wintypes.LPVOID,
        wintypes.LPVOID,
        wintypes.BOOL,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.LPCWSTR,
        ctypes.POINTER(STARTUPINFOW),
        ctypes.POINTER(PROCESS_INFORMATION),
    ]
    kernel32.CreateProcessW.restype = wintypes.BOOL
    kernel32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.SetInformationJobObject.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
    ]
    kernel32.SetInformationJobObject.restype = wintypes.BOOL
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    kernel32.SetProcessAffinityMask.restype = wintypes.BOOL
    kernel32.GetProcessAffinityMask.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.GetProcessAffinityMask.restype = wintypes.BOOL
    kernel32.GetCurrentProcess.argtypes = []
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.IsProcessInJob.argtypes = [
        wintypes.HANDLE,
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.BOOL),
    ]
    kernel32.IsProcessInJob.restype = wintypes.BOOL
    kernel32.QueryInformationJobObject.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.QueryInformationJobObject.restype = wintypes.BOOL
    kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
    kernel32.ResumeThread.restype = wintypes.DWORD
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    ]
    kernel32.GetProcessTimes.restype = wintypes.BOOL
    kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.TerminateJobObject.restype = wintypes.BOOL
    kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.TerminateProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL


def _winerr(prefix: str) -> PerfHostError:
    return PerfHostError(f"{prefix}: WinError {ctypes.get_last_error()}")


def validate_timeout(value: float | None) -> float | None:
    if value is None:
        return None
    if not math.isfinite(value) or value < 0.0:
        raise ValueError("--timeout must be a finite non-negative number")
    if value * 1000.0 > 0xFFFFFFFE:
        raise ValueError("--timeout is too large for the Windows wait API")
    return value


def affinity_scaled_cpu_rate_units(
    cpu_percent: float | None,
    affinity_mask: int | None,
    logical_cpu_count: int | None,
) -> int | None:
    if cpu_percent is None or not logical_cpu_count:
        return None
    affinity_cpus = affinity_mask.bit_count() if affinity_mask is not None else logical_cpu_count
    if affinity_cpus <= 0:
        return None
    units = math.ceil(cpu_percent * affinity_cpus * 100 / logical_cpu_count)
    return min(10000, max(1, units))


def one_core_cpu_rate_units(core_percent: float | None, logical_cpu_count: int | None) -> int | None:
    if core_percent is None or not logical_cpu_count:
        return None
    units = math.ceil(core_percent * 100 / logical_cpu_count)
    return min(10000, max(1, units))


def host_cpu_rate_units(cpu_percent: int | None) -> int | None:
    if cpu_percent is None:
        return None
    return min(10000, max(1, cpu_percent * 100))


def _path_text(path: Path | None) -> str | None:
    return str(path) if path is not None else None


def _set_job_kill_on_close(job: int) -> None:
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    ok = kernel32.SetInformationJobObject(
        job,
        JobObjectExtendedLimitInformation,
        ctypes.byref(info),
        ctypes.sizeof(info),
    )
    if not ok:
        raise _winerr("SetInformationJobObject(KILL_ON_JOB_CLOSE) failed")


def _set_job_cpu_rate(job: int, cpu_rate: int) -> int:
    info = JOBOBJECT_CPU_RATE_CONTROL_INFORMATION()
    info.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP
    info.CpuRate = cpu_rate
    ok = kernel32.SetInformationJobObject(
        job,
        JobObjectCpuRateControlInformation,
        ctypes.byref(info),
        ctypes.sizeof(info),
    )
    if not ok:
        raise _winerr("SetInformationJobObject(CPU_RATE_CONTROL) failed")
    return cpu_rate


def _resolve_application(argv: Sequence[str]) -> str | None:
    if not argv:
        raise ValueError("missing executable after --")
    exe = argv[0]
    if any(ch in exe for ch in "\\/:"):
        return exe
    found = shutil.which(exe)
    return found


def _query_windows_affinity(process: int) -> dict[str, int] | None:
    if not hasattr(kernel32, "GetProcessAffinityMask"):
        return None
    proc_mask = ctypes.c_size_t()
    sys_mask = ctypes.c_size_t()
    ok = kernel32.GetProcessAffinityMask(
        process, ctypes.byref(proc_mask), ctypes.byref(sys_mask)
    )
    if not ok:
        return None
    return {"process_mask": int(proc_mask.value), "system_mask": int(sys_mask.value)}


def _validate_windows_affinity_mask(affinity_mask: int | None) -> None:
    if affinity_mask is None:
        return
    size_t_bits = ctypes.sizeof(ctypes.c_size_t) * 8
    if affinity_mask.bit_length() > size_t_bits:
        raise ValueError(
            f"affinity mask 0x{affinity_mask:x} exceeds {size_t_bits}-bit size_t"
        )
    current_process = kernel32.GetCurrentProcess() if hasattr(kernel32, "GetCurrentProcess") else None
    current_affinity = (
        _query_windows_affinity(current_process) if current_process is not None else None
    )
    system_mask = current_affinity["system_mask"] if current_affinity else None
    if system_mask is not None and affinity_mask & ~system_mask:
        raise ValueError(
            f"affinity mask 0x{affinity_mask:x} is outside system mask 0x{system_mask:x}"
        )


def _is_process_in_job(process: int, job: int | None = None) -> bool | None:
    if not hasattr(kernel32, "IsProcessInJob"):
        return None
    result = wintypes.BOOL()
    ok = kernel32.IsProcessInJob(process, job, ctypes.byref(result))
    if not ok:
        return None
    return bool(result.value)


def _query_job_cpu_rate(job: int) -> dict[str, int] | None:
    if not hasattr(kernel32, "QueryInformationJobObject"):
        return None
    info = JOBOBJECT_CPU_RATE_CONTROL_INFORMATION()
    returned = wintypes.DWORD()
    ok = kernel32.QueryInformationJobObject(
        job,
        JobObjectCpuRateControlInformation,
        ctypes.byref(info),
        ctypes.sizeof(info),
        ctypes.byref(returned),
    )
    if not ok:
        return None
    return {"control_flags": int(info.ControlFlags), "cpu_rate_units": int(info.CpuRate)}


def _open_windows_stdio(
    stdout_log: Path | None,
    stderr_log: Path | None,
) -> tuple[list[Any], dict[str, Any]]:
    files: list[Any] = []
    info = {
        "stdout_log": _path_text(stdout_log),
        "stderr_log": _path_text(stderr_log),
        "stdio_mode": "log-files" if stdout_log or stderr_log else "detached",
    }
    if not stdout_log and not stderr_log:
        return files, info

    import msvcrt

    stdin = open(os.devnull, "rb")
    files.append(stdin)
    os.set_handle_inheritable(msvcrt.get_osfhandle(stdin.fileno()), True)

    if stdout_log:
        stdout_log.parent.mkdir(parents=True, exist_ok=True)
        stdout = open(stdout_log, "wb")
        files.append(stdout)
    else:
        stdout = open(os.devnull, "wb")
        files.append(stdout)
    os.set_handle_inheritable(msvcrt.get_osfhandle(stdout.fileno()), True)

    if stderr_log:
        stderr_log.parent.mkdir(parents=True, exist_ok=True)
        mode = "ab" if stdout_log and stderr_log == stdout_log else "wb"
        stderr = open(stderr_log, mode)
        files.append(stderr)
    else:
        stderr = open(os.devnull, "wb")
        files.append(stderr)
    os.set_handle_inheritable(msvcrt.get_osfhandle(stderr.fileno()), True)

    info["stdin_handle"] = "NUL"
    return files, info


def launch_windows(
    argv: Sequence[str],
    cpu_percent: int | None,
    affinity_mask: int | None,
    timeout: float | None,
    cwd: str | None = None,
    stdout_log: Path | None = None,
    stderr_log: Path | None = None,
    cpu_rate_units: int | None = None,
    cpu_rate_mode: str = "host_percent",
    core_percent: float | None = None,
) -> tuple[LaunchResult, dict[str, Any]]:
    if not argv:
        raise ValueError("missing executable after --")
    _validate_windows_affinity_mask(affinity_mask)

    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.dwFlags = STARTF_USESHOWWINDOW
    si.wShowWindow = SW_HIDE
    stdio_files, stdio_info = _open_windows_stdio(stdout_log, stderr_log)
    if stdio_files:
        import msvcrt

        si.dwFlags |= STARTF_USESTDHANDLES
        si.hStdInput = msvcrt.get_osfhandle(stdio_files[0].fileno())
        si.hStdOutput = msvcrt.get_osfhandle(stdio_files[1].fileno())
        si.hStdError = msvcrt.get_osfhandle(stdio_files[2].fileno())
    pi = PROCESS_INFORMATION()
    cmdline = ctypes.create_unicode_buffer(subprocess.list2cmdline(list(argv)))
    app = _resolve_application(argv)
    creation_flags = CREATE_SUSPENDED | CREATE_NO_WINDOW
    start = time.perf_counter()
    try:
        ok = kernel32.CreateProcessW(
            app,
            cmdline,
            None,
            None,
            bool(stdio_files),
            creation_flags,
            None,
            cwd,
            ctypes.byref(si),
            ctypes.byref(pi),
        )
        if not ok:
            raise _winerr("CreateProcessW failed")
    except Exception:
        for f in stdio_files:
            f.close()
        raise

    job = None
    logical_cpu_count = os.cpu_count()
    if cpu_rate_units is None:
        cpu_rate_units = host_cpu_rate_units(cpu_percent)
    current_process = kernel32.GetCurrentProcess() if hasattr(kernel32, "GetCurrentProcess") else None
    parent_in_job = (
        _is_process_in_job(current_process, None) if current_process is not None else None
    )
    child_in_job_before_assign = _is_process_in_job(pi.hProcess, None)
    effective: dict[str, Any] = {
        "platform": "windows",
        "throttle_supported": True,
        "cpu_rate_mode": cpu_rate_mode,
        "core_percent": core_percent,
        "job_cpu_rate_units": None,
        "job_cpu_rate_denominator": 10000,
        "job_cpu_rate_readback": None,
        "job_denominator_note": (
            "Windows Job Object CpuRate units; 10000 is 100 percent of the "
            "OS job CPU-rate denominator, not a single-thread slowdown model. "
            "Record affinity separately and calibrate with a one-worker "
            "CPU-bound witness on the target host."
        ),
        "parent_process_in_job": parent_in_job,
        "child_process_in_job_before_assign": child_in_job_before_assign,
        "nested_job_note": (
            "If a parent job already has CPU rate control, Windows applies "
            "nested job rates relative to that parent. This report records "
            "whether the host and child were already in a job before assignment."
        ),
        "affinity_cpu_count": affinity_mask.bit_count() if affinity_mask is not None else None,
        "affinity_scaled_job_cpu_rate_units": affinity_scaled_cpu_rate_units(
            cpu_percent, affinity_mask, logical_cpu_count
        ),
        "core_percent_job_cpu_rate_units": one_core_cpu_rate_units(
            core_percent, logical_cpu_count
        ),
        "affinity_scaled_note": (
            "Informational only: raw Job Object CpuRate units that would make "
            "--cpu-percent apply to the requested affinity set as a fraction "
            "of this host's logical CPU count. --core-percent instead applies "
            "a one-logical-CPU-equivalent conversion, independent of affinity."
        ),
        "affinity_mask": affinity_mask,
        "affinity_applied": False,
        "affinity_actual": None,
        **stdio_info,
    }
    timed_out = False
    exit_code: int | None = None
    user_s = kernel_s = total_s = None
    try:
        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            raise _winerr("CreateJobObjectW failed")
        _set_job_kill_on_close(job)
        if cpu_rate_units is not None:
            effective["job_cpu_rate_units"] = _set_job_cpu_rate(job, cpu_rate_units)
            effective["job_cpu_rate_readback"] = _query_job_cpu_rate(job)
        if affinity_mask is not None:
            if not kernel32.SetProcessAffinityMask(pi.hProcess, affinity_mask):
                raise _winerr("SetProcessAffinityMask failed")
            effective["affinity_applied"] = True
        effective["affinity_actual"] = _query_windows_affinity(pi.hProcess)
        if not kernel32.AssignProcessToJobObject(job, pi.hProcess):
            raise _winerr("AssignProcessToJobObject failed")
        if kernel32.ResumeThread(pi.hThread) == 0xFFFFFFFF:
            raise _winerr("ResumeThread failed")

        wait_ms = INFINITE if timeout is None else max(0, int(timeout * 1000))
        wait = kernel32.WaitForSingleObject(pi.hProcess, wait_ms)
        if wait == WAIT_TIMEOUT:
            timed_out = True
            kernel32.TerminateJobObject(job, 1)
            done = kernel32.WaitForSingleObject(pi.hProcess, TERMINATE_WAIT_MS)
            if done == WAIT_TIMEOUT:
                raise PerfHostError("child did not exit after timeout termination")
        elif wait != WAIT_OBJECT_0:
            raise _winerr("WaitForSingleObject failed")

        code = wintypes.DWORD()
        if not kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code)):
            raise _winerr("GetExitCodeProcess failed")
        exit_code = int(code.value)

        creation = wintypes.FILETIME()
        exit_ft = wintypes.FILETIME()
        kernel_ft = wintypes.FILETIME()
        user_ft = wintypes.FILETIME()
        if kernel32.GetProcessTimes(
            pi.hProcess,
            ctypes.byref(creation),
            ctypes.byref(exit_ft),
            ctypes.byref(kernel_ft),
            ctypes.byref(user_ft),
        ):
            user_s = _filetime_seconds(user_ft)
            kernel_s = _filetime_seconds(kernel_ft)
            total_s = user_s + kernel_s
    except Exception:
        if job:
            kernel32.TerminateJobObject(job, 1)
        if pi.hProcess:
            kernel32.TerminateProcess(pi.hProcess, 1)
            kernel32.WaitForSingleObject(pi.hProcess, TERMINATE_WAIT_MS)
        raise
    finally:
        if pi.hThread:
            kernel32.CloseHandle(pi.hThread)
        if pi.hProcess:
            kernel32.CloseHandle(pi.hProcess)
        if job:
            kernel32.CloseHandle(job)
        for f in stdio_files:
            f.close()

    result = LaunchResult(
        pid=int(pi.dwProcessId),
        exit_code=exit_code,
        timed_out=timed_out,
        wall_seconds=time.perf_counter() - start,
        cpu_user_seconds=user_s,
        cpu_kernel_seconds=kernel_s,
        cpu_total_seconds=total_s,
    )
    return result, effective


def launch_portable(
    argv: Sequence[str],
    cpu_percent: int | None,
    affinity_mask: int | None,
    timeout: float | None,
    cwd: str | None = None,
    stdout_log: Path | None = None,
    stderr_log: Path | None = None,
    cpu_rate_units: int | None = None,
    cpu_rate_mode: str = "host_percent",
    core_percent: float | None = None,
) -> tuple[LaunchResult, dict[str, Any]]:
    if cpu_percent is not None or cpu_rate_units is not None or core_percent is not None:
        raise UnsupportedThrottle("CPU throttling is only supported on Windows via Job Objects")
    if affinity_mask is not None and not hasattr(os, "sched_setaffinity"):
        raise UnsupportedThrottle("affinity is unsupported on this platform")

    def set_affinity() -> None:
        if affinity_mask is not None and hasattr(os, "sched_setaffinity"):
            cpus = {i for i in range(affinity_mask.bit_length()) if affinity_mask & (1 << i)}
            os.sched_setaffinity(0, cpus)  # type: ignore[attr-defined]

    start = time.perf_counter()
    stdout_f = None
    stderr_f = None
    stdout_target: Any = None
    stderr_target: Any = None
    if stdout_log:
        stdout_log.parent.mkdir(parents=True, exist_ok=True)
        stdout_f = open(stdout_log, "wb")
        stdout_target = stdout_f
    if stderr_log:
        stderr_log.parent.mkdir(parents=True, exist_ok=True)
        stderr_f = open(stderr_log, "ab" if stdout_log and stderr_log == stdout_log else "wb")
        stderr_target = stderr_f
    proc = subprocess.Popen(
        list(argv),
        cwd=cwd,
        preexec_fn=set_affinity if affinity_mask is not None and os.name == "posix" else None,
        start_new_session=os.name == "posix",
        stdout=stdout_target,
        stderr=stderr_target,
    )
    timed_out = False
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGKILL)
        else:
            proc.kill()
        proc.wait()
    except BaseException:
        if proc.poll() is None:
            if os.name == "posix":
                os.killpg(proc.pid, signal.SIGKILL)
            else:
                proc.kill()
            proc.wait()
        raise
    finally:
        if stdout_f:
            stdout_f.close()
        if stderr_f:
            stderr_f.close()
    result = LaunchResult(
        pid=proc.pid,
        exit_code=proc.returncode,
        timed_out=timed_out,
        wall_seconds=time.perf_counter() - start,
        cpu_user_seconds=None,
        cpu_kernel_seconds=None,
        cpu_total_seconds=None,
    )
    return result, {
        "platform": "portable",
        "throttle_supported": False,
        "cpu_rate_mode": cpu_rate_mode,
        "core_percent": core_percent,
        "job_cpu_rate_units": None,
        "job_cpu_rate_denominator": None,
        "affinity_mask": affinity_mask,
        "affinity_applied": affinity_mask is not None and hasattr(os, "sched_setaffinity"),
        "stdout_log": _path_text(stdout_log),
        "stderr_log": _path_text(stderr_log),
        "stdio_mode": "log-files" if stdout_log or stderr_log else "inherit",
    }


def run_host(
    argv: Sequence[str],
    cpu_percent: int | None,
    affinity_mask: int | None,
    timeout: float | None,
    cwd: str | None = None,
    stdout_log: Path | None = None,
    stderr_log: Path | None = None,
    core_percent: float | None = None,
) -> dict[str, Any]:
    cpu_percent = validate_cpu_percent(cpu_percent)
    core_percent = validate_core_percent(core_percent)
    if cpu_percent is not None and core_percent is not None:
        raise ValueError("--cpu-percent and --core-percent are mutually exclusive")
    timeout = validate_timeout(timeout)
    cpu_rate_mode = "host_percent"
    cpu_rate_units = host_cpu_rate_units(cpu_percent)
    if core_percent is not None:
        cpu_rate_mode = "one_core_percent"
        cpu_rate_units = one_core_cpu_rate_units(core_percent, os.cpu_count())
    launcher = launch_windows if os.name == "nt" else launch_portable
    result, effective = launcher(
        argv,
        cpu_percent,
        affinity_mask,
        timeout,
        cwd,
        stdout_log,
        stderr_log,
        cpu_rate_units,
        cpu_rate_mode,
        core_percent,
    )
    return {
        "schema_version": 1,
        "host": topology_report(),
        "request": {
            "cpu_percent": cpu_percent,
            "core_percent": core_percent,
            "affinity": affinity_mask,
            "timeout_seconds": timeout,
            "argv": list(argv),
            "cwd": cwd,
            "stdout_log": _path_text(stdout_log),
            "stderr_log": _path_text(stderr_log),
        },
        "effective": effective,
        "process": {
            "pid": result.pid,
            "exit_code": result.exit_code,
            "timed_out": result.timed_out,
            "wall_seconds": result.wall_seconds,
            "cpu_user_seconds": result.cpu_user_seconds,
            "cpu_kernel_seconds": result.cpu_kernel_seconds,
            "cpu_total_seconds": result.cpu_total_seconds,
        },
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run a command under a process-scoped host CPU budget."
    )
    parser.add_argument("--cpu-percent", type=int, default=None,
                        help="Windows Job Object hard CPU cap, 1..100 percent.")
    parser.add_argument("--core-percent", type=float, default=None,
                        help=(
                            "Windows cap as percent of one logical CPU; "
                            "converts to raw Job Object units using host "
                            "logical CPU count, independent of affinity."
                        ))
    parser.add_argument("--affinity", default=None,
                        help="Affinity mask, e.g. 0x3, or CPU list, e.g. 0,1.")
    parser.add_argument("--timeout", type=float, default=None,
                        help="Seconds before terminating only the launched job/process.")
    parser.add_argument("--report", type=Path, default=None,
                        help="Write JSON report to this path. Defaults to stdout.")
    parser.add_argument("--stdout-log", type=Path, default=None,
                        help="Write child stdout to this path.")
    parser.add_argument("--stderr-log", type=Path, default=None,
                        help="Write child stderr to this path.")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="Command to run after --.")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    ns = parser.parse_args(argv)
    command = list(ns.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("missing command after --")
    stdout_log = ns.stdout_log
    stderr_log = ns.stderr_log
    if ns.report:
        if stdout_log is None:
            stdout_log = ns.report.with_name(ns.report.name + ".stdout.log")
        if stderr_log is None:
            stderr_log = ns.report.with_name(ns.report.name + ".stderr.log")

    try:
        report = run_host(
            command,
            ns.cpu_percent,
            parse_affinity(ns.affinity),
            ns.timeout,
            os.getcwd(),
            stdout_log,
            stderr_log,
            ns.core_percent,
        )
    except (PerfHostError, OSError, ValueError) as exc:
        error = {
            "schema_version": 1,
            "host": topology_report(),
            "request": {
                "cpu_percent": ns.cpu_percent,
                "core_percent": ns.core_percent,
                "affinity": ns.affinity,
                "timeout_seconds": ns.timeout,
                "argv": command,
                "cwd": os.getcwd(),
                "stdout_log": _path_text(stdout_log),
                "stderr_log": _path_text(stderr_log),
            },
            "error": str(exc),
        }
        body = json.dumps(error, indent=2, sort_keys=True)
        if ns.report:
            ns.report.parent.mkdir(parents=True, exist_ok=True)
            ns.report.write_text(body + "\n", encoding="utf-8")
        else:
            print(body)
        return 2

    body = json.dumps(report, indent=2, sort_keys=True)
    if ns.report:
        ns.report.parent.mkdir(parents=True, exist_ok=True)
        ns.report.write_text(body + "\n", encoding="utf-8")
    else:
        print(body)
    return int(report["process"]["exit_code"] or 0)


if __name__ == "__main__":
    raise SystemExit(main())
