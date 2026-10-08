"""Execute the production TCP handler/helpers and real ring through the C ABI.

No server, game, SDL, or persistent log. Missing compilers are an explicit skip.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_wtrace_dump import matching_brace

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^(?:static |extern "C" )[^;{}]+\b' + name +
                      r'\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f"production function missing: {name}")
    return source[match.start():matching_brace(source, match.end() - 1) + 1]


class HostLaunchProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc, cxx = os.environ.get("CC", "cc"), os.environ.get("CXX", "c++")
        if not shutil.which(cc) or not shutil.which(cxx):
            raise unittest.SkipTest("set CC and CXX to the native compilers")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        debug = (ROOT / "runtime/src/debug_server.c").read_text(encoding="utf-8")
        runtime = (ROOT / "runtime/src/mod_runtime.cpp").read_text(encoding="utf-8")
        harness = '''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_launch_timing.h"
void seed_host_launch_timing_fixture(int count);
static void debug_server_send_line(const char *line) { puts(line); }
static void send_err(int id, const char *error) { (void)id; (void)error; abort(); }
'''
        for name in ("json_get_int", "json_escape_string", "handle_host_launch_timings"):
            harness += function(debug, name) + "\n"
        harness += '''int main(int argc, char **argv) {
    if (argc != 3) return 2;
    seed_host_launch_timing_fixture(atoi(argv[2]));
    handle_host_launch_timings(42, argv[1]);
    return 0;
}
'''
        bridge = '#include "host_launch_timing.h"\n'
        for name in ("host_launch_timing_snapshot", "host_launch_timing_stage_name"):
            bridge += function(runtime, name) + "\n"
        bridge += '''extern "C" void seed_host_launch_timing_fixture(int count) {
    char label[64];
    for (unsigned i = 0; i < 63; ++i) label[i] = i % 3 == 0 ? '"' : i % 3 == 1 ? '\\\\' : '\\1';
    label[63] = 0;
    for (int i = 0; i < count; ++i) {
        PSXRecompV4::HostLaunchTimingScope scope(HOST_LAUNCH_MEDIA_PROVIDER, label, label);
        if (i % 2 == 0) scope.success();
    }
}
'''
        (temp / "handler.c").write_text(harness, encoding="utf-8")
        (temp / "bridge.cpp").write_text(bridge, encoding="utf-8")
        include = str(ROOT / "runtime/include")
        cls.exe = temp / ("protocol.exe" if os.name == "nt" else "protocol")
        for args in ([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", include,
                      "-c", str(temp / "handler.c"), "-o", str(temp / "handler.o")],
                     [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                      "-I", include, str(temp / "bridge.cpp"), str(temp / "handler.o"),
                      "-o", str(cls.exe)]):
            subprocess.run(args, check=True, capture_output=True, text=True)

    def query(self, count=None, seeded=300):
        request = {"cmd": "host_launch_timings"}
        if count is not None:
            request["count"] = count
        output = subprocess.check_output([str(self.exe), json.dumps(request), str(seeded)], text=True)
        return json.loads(output)

    def test_wrapped_maximum_labels_and_default_count(self):
        result = self.query()
        self.assertEqual((result["capacity"], result["total"], result["overwritten"]), (256, 300, 44))
        self.assertTrue(result["nested_durations_overlap"])
        self.assertEqual([e["seq"] for e in result["entries"]], list(range(45, 301)))
        for e in result["entries"]:
            self.assertEqual(e["stage"], "media_provider")
            self.assertEqual(e["package"], '"\\\x01' * 21)
            self.assertEqual(e["feature"], e["package"])
            self.assertEqual(e["labels_truncated"], 0)
            self.assertEqual(e["ok"], int(e["seq"] % 2 == 1))

    def test_count_clamping(self):
        for count, expected in ((0, 1), (-10, 1), (1, 1), (3, 3), (256, 256), (1000, 256)):
            result = self.query(count)
            self.assertEqual(len(result["entries"]), expected)
            self.assertEqual(result["entries"][-1]["seq"], 300)

    def test_empty_ring(self):
        result = self.query(seeded=0)
        self.assertEqual((result["total"], result["overwritten"], result["entries"]), (0, 0, []))


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HostLaunchProtocol)
    result = unittest.TextTestRunner().run(suite)
    raise SystemExit(77 if result.skipped else 0 if result.wasSuccessful() else 1)
