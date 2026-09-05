#!/usr/bin/env python3
"""Tests for perf_host.py."""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("perf_host", ROOT / "tools" / "perf_host.py")
PERF = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules["perf_host"] = PERF
SPEC.loader.exec_module(PERF)


class PerfHostTests(unittest.TestCase):
    def test_parse_affinity_accepts_mask_and_list(self):
        self.assertEqual(PERF.parse_affinity("0x3"), 0x3)
        self.assertEqual(PERF.parse_affinity("1,3"), 0b1010)
        self.assertIsNone(PERF.parse_affinity(None))

    def test_parse_affinity_rejects_zero_and_negative_index(self):
        with self.assertRaises(ValueError):
            PERF.parse_affinity("0")
        with self.assertRaises(ValueError):
            PERF.parse_affinity("0,-1")

    def test_validate_cpu_percent_range(self):
        self.assertEqual(PERF.validate_cpu_percent(25), 25)
        self.assertIsNone(PERF.validate_cpu_percent(None))
        with self.assertRaises(ValueError):
            PERF.validate_cpu_percent(0)
        with self.assertRaises(ValueError):
            PERF.validate_cpu_percent(101)

    def test_validate_core_percent_range(self):
        self.assertEqual(PERF.validate_core_percent(12.5), 12.5)
        self.assertIsNone(PERF.validate_core_percent(None))
        with self.assertRaises(ValueError):
            PERF.validate_core_percent(0.0)
        with self.assertRaises(ValueError):
            PERF.validate_core_percent(float("inf"))

    def test_affinity_scaled_cpu_rate_units(self):
        self.assertEqual(PERF.affinity_scaled_cpu_rate_units(50, 0x1, 16), 313)
        self.assertEqual(PERF.affinity_scaled_cpu_rate_units(25, 0x3, 8), 625)
        self.assertEqual(PERF.affinity_scaled_cpu_rate_units(50, None, 16), 5000)
        self.assertIsNone(PERF.affinity_scaled_cpu_rate_units(None, 0x1, 16))
        self.assertEqual(PERF.one_core_cpu_rate_units(50, 16), 313)
        self.assertEqual(PERF.one_core_cpu_rate_units(25, 16), 157)

    def test_core_percent_computes_raw_rate_units(self):
        fake = PERF.LaunchResult(
            pid=123,
            exit_code=0,
            timed_out=False,
            wall_seconds=0.125,
            cpu_user_seconds=None,
            cpu_kernel_seconds=None,
            cpu_total_seconds=None,
        )
        with mock.patch.object(PERF, "launch_windows",
                               return_value=(fake, {"platform": "windows"})) as launcher, \
                mock.patch.object(PERF.os, "name", "nt"), \
                mock.patch.object(PERF.os, "cpu_count", return_value=16):
            PERF.run_host(["tool"], None, 0x1, 1.0, core_percent=50)
        self.assertEqual(launcher.call_args.args[7], 313)
        self.assertEqual(launcher.call_args.args[8], "one_core_percent")

    def test_cpu_percent_and_core_percent_are_mutually_exclusive(self):
        with self.assertRaises(ValueError):
            PERF.run_host(["tool"], 50, 0x1, 1.0, core_percent=50)

    def test_portable_throttle_fails_honestly(self):
        with self.assertRaises(PERF.UnsupportedThrottle):
            PERF.launch_portable([sys.executable, "-c", "pass"], 25, None, 1.0)

    def test_run_host_records_report_shape_with_mock_launcher(self):
        fake = PERF.LaunchResult(
            pid=123,
            exit_code=7,
            timed_out=False,
            wall_seconds=0.125,
            cpu_user_seconds=0.01,
            cpu_kernel_seconds=0.02,
            cpu_total_seconds=0.03,
        )
        with mock.patch.object(PERF, "launch_portable",
                               return_value=(fake, {"platform": "portable"})), \
                mock.patch.object(PERF.os, "name", "posix"):
            report = PERF.run_host(["tool", "arg"], None, 0x3, 2.0, "cwd")
        self.assertEqual(report["request"]["argv"], ["tool", "arg"])
        self.assertEqual(report["request"]["affinity"], 0x3)
        self.assertEqual(report["effective"]["platform"], "portable")
        self.assertEqual(report["process"]["exit_code"], 7)

    def test_main_writes_error_report_for_unsupported_throttle(self):
        with tempfile.TemporaryDirectory() as td:
            report_path = Path(td) / "report.json"
            with mock.patch.object(
                PERF,
                "run_host",
                side_effect=PERF.UnsupportedThrottle(
                    "CPU throttling is only supported on Windows via Job Objects"
                ),
            ):
                rc = PERF.main([
                    "--cpu-percent", "50",
                    "--report", str(report_path),
                    "--",
                    sys.executable, "-c", "pass",
                ])
            body = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(rc, 2)
        self.assertIn("CPU throttling is only supported", body["error"])

    def test_windows_failure_terminates_scoped_job(self):
        if os.name != "nt":
            self.skipTest("Windows API layer is only defined on Windows")

        calls: list[str] = []

        class FakeKernel:
            def CreateProcessW(self, app, cmd, psa, tsa, inherit, flags, env, cwd, si, pi):
                pi._obj.hProcess = 100
                pi._obj.hThread = 200
                pi._obj.dwProcessId = 300
                return True

            def CreateJobObjectW(self, attrs, name):
                return 400

            def SetInformationJobObject(self, *args):
                return True

            def SetProcessAffinityMask(self, *args):
                return True

            def GetProcessAffinityMask(self, process, process_mask, system_mask):
                process_mask._obj.value = 0x3
                system_mask._obj.value = 0xFFFF
                return True

            def AssignProcessToJobObject(self, *args):
                calls.append("assign")
                return False

            def ResumeThread(self, *args):
                calls.append("resume")
                return 0

            def TerminateJobObject(self, *args):
                calls.append("terminate_job")
                return True

            def TerminateProcess(self, *args):
                calls.append("terminate_process")
                return True

            def WaitForSingleObject(self, *args):
                calls.append("wait")
                return PERF.WAIT_OBJECT_0

            def CloseHandle(self, handle):
                calls.append(f"close:{handle}")
                return True

        with mock.patch.object(PERF, "kernel32", FakeKernel()), \
                mock.patch.object(PERF.ctypes, "get_last_error", return_value=5):
            with self.assertRaisesRegex(PERF.PerfHostError, "AssignProcessToJobObject"):
                PERF.launch_windows([sys.executable, "-c", "pass"], 50, None, 1.0)
        self.assertIn("terminate_job", calls)
        self.assertIn("terminate_process", calls)
        self.assertNotIn("resume", calls)

    def test_windows_get_exit_code_failure_terminates_scoped_job(self):
        if os.name != "nt":
            self.skipTest("Windows API layer is only defined on Windows")

        calls: list[str] = []

        class FakeKernel:
            def CreateProcessW(self, app, cmd, psa, tsa, inherit, flags, env, cwd, si, pi):
                pi._obj.hProcess = 100
                pi._obj.hThread = 200
                pi._obj.dwProcessId = 300
                return True

            def CreateJobObjectW(self, attrs, name):
                return 400

            def SetInformationJobObject(self, *args):
                return True

            def AssignProcessToJobObject(self, *args):
                return True

            def ResumeThread(self, *args):
                calls.append("resume")
                return 0

            def WaitForSingleObject(self, *args):
                return PERF.WAIT_OBJECT_0

            def GetExitCodeProcess(self, *args):
                return False

            def TerminateJobObject(self, *args):
                calls.append("terminate_job")
                return True

            def TerminateProcess(self, *args):
                calls.append("terminate_process")
                return True

            def CloseHandle(self, handle):
                calls.append(f"close:{handle}")
                return True

        with mock.patch.object(PERF, "kernel32", FakeKernel()), \
                mock.patch.object(PERF.ctypes, "get_last_error", return_value=6):
            with self.assertRaisesRegex(PERF.PerfHostError, "GetExitCodeProcess"):
                PERF.launch_windows([sys.executable, "-c", "pass"], None, None, 1.0)
        self.assertIn("resume", calls)
        self.assertIn("terminate_job", calls)
        self.assertIn("terminate_process", calls)

    def test_windows_create_process_failure_closes_stdio_files(self):
        if os.name != "nt":
            self.skipTest("Windows API layer is only defined on Windows")

        class FakeKernel:
            def CreateProcessW(self, *args):
                return False

        with tempfile.TemporaryDirectory() as td, \
                mock.patch.object(PERF, "kernel32", FakeKernel()), \
                mock.patch.object(PERF.ctypes, "get_last_error", return_value=2):
            out = Path(td) / "child.out"
            err = Path(td) / "child.err"
            with self.assertRaisesRegex(PERF.PerfHostError, "CreateProcessW"):
                PERF.launch_windows([sys.executable, "-c", "pass"], None, None, 1.0,
                                    stdout_log=out, stderr_log=err)
            out.unlink()
            err.unlink()

    @unittest.skipIf(os.name != "nt", "Windows Job Object test")
    def test_windows_real_child_unrestricted(self):
        report = PERF.run_host([sys.executable, "-c", "import sys; sys.exit(3)"],
                               None, None, 5.0)
        self.assertEqual(report["process"]["exit_code"], 3)
        self.assertFalse(report["process"]["timed_out"])
        self.assertEqual(report["effective"]["platform"], "windows")

    @unittest.skipIf(os.name != "nt", "Windows Job Object test")
    def test_windows_real_child_preserves_logs(self):
        with tempfile.TemporaryDirectory() as td:
            report_path = Path(td) / "report.json"
            rc = PERF.main([
                "--timeout", "5",
                "--report", str(report_path),
                "--",
                sys.executable, "-c",
                "import sys; print('out'); print('err', file=sys.stderr); sys.exit(3)",
            ])
            body = json.loads(report_path.read_text(encoding="utf-8"))
            stdout_log = Path(body["effective"]["stdout_log"])
            stderr_log = Path(body["effective"]["stderr_log"])
            self.assertEqual(rc, 3)
            self.assertEqual(stdout_log.read_text(encoding="utf-8").strip(), "out")
            self.assertEqual(stderr_log.read_text(encoding="utf-8").strip(), "err")

    @unittest.skipIf(os.name != "nt", "Windows Job Object test")
    def test_windows_real_child_exit_code_259_is_preserved(self):
        report = PERF.run_host([sys.executable, "-c", "import sys; sys.exit(259)"],
                               None, None, 5.0)
        self.assertEqual(report["process"]["exit_code"], 259)
        self.assertFalse(report["process"]["timed_out"])

    @unittest.skipIf(os.name != "nt", "Windows Job Object test")
    def test_windows_real_child_timeout_with_cap(self):
        report = PERF.run_host([sys.executable, "-c", "while True: pass"],
                               25, None, 0.2)
        self.assertTrue(report["process"]["timed_out"])
        self.assertEqual(report["effective"]["job_cpu_rate_units"], 2500)


if __name__ == "__main__":
    unittest.main()
