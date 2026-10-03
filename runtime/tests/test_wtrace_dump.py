"""Execute the production trace-query handler against a wrapped fixture ring.

Compile only the handler and its real JSON helpers, replacing the transport
with stdout. This needs a C compiler, but no game image, SDL or live server.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


class WriteTraceQuery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = (Path(__file__).parents[1] / "src/debug_server.c").read_text(encoding="utf-8")

        def function(name):
            match = re.search(r"static [^;{}]+\b" + name + r"\([^;{}]*\)\s*\{", source)
            if not match:
                raise AssertionError(f"production function missing: {name}")
            end = source.index("\n}", match.start()) + 2
            return source[match.start():end]

        end = source.index("} WriteTraceEntry;") + len("} WriteTraceEntry;")
        start = source.rfind("typedef struct {", 0, end)
        harness = """#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WRITE_TRACE_CAP 8
""" + source[start:end] + """
static WriteTraceEntry ring[WRITE_TRACE_CAP], *s_wtrace=ring;
static uint64_t s_wtrace_seq=10;
static uint32_t s_wtrace_head=2;
static void debug_server_send_line(const char *line) { puts(line); }
static void send_err(int id, const char *error) { (void)id; (void)error; abort(); }
"""
        for name in ("json_get_str", "json_get_int", "hex_to_u32", "handle_wtrace_dump"):
            harness += function(name) + "\n"
        harness += """int main(int argc, char **argv) {
    const uint32_t pcs[8]={0x80001000,0x80001004,0xA0001000,0x80001008,
                           0x8000100C,0x80001010,0x80001004,0x00001004};
    for (int seq=2; seq<10; ++seq) {
        WriteTraceEntry *e=&ring[seq%WRITE_TRACE_CAP];
        e->seq=seq; e->pc=pcs[seq-2]; e->addr=0x1000+(seq-2)*4;
        e->frame=10+(seq-2)/2; e->width=4; e->dma_ch=seq==5?2:-1;
    }
    handle_wtrace_dump(7,argc>1?argv[1]:"{}");
    return 0;
}
"""
        path = Path(cls.temp.name)
        (path / "query.c").write_text(harness, encoding="utf-8", newline="\n")
        cls.exe = path / ("query.exe" if os.name == "nt" else "query")
        compiler_setting = os.environ.get("CC", "cc")
        compiler = ([compiler_setting] if shutil.which(compiler_setting) else
                    shlex.split(compiler_setting, posix=os.name != "nt"))
        if not shutil.which(compiler[0]):
            raise RuntimeError("Set CC to a C compiler to run the trace-query regression")
        subprocess.run([*compiler, str(path / "query.c"), "-o", str(cls.exe)], check=True)

    def query(self, **params):
        result = subprocess.check_output([str(self.exe), json.dumps(params)], text=True)
        reply = json.loads(result)
        self.assertEqual(reply["emitted"], len(reply["entries"]))
        return reply

    def sequences(self, **params):
        return [e["seq"] for e in self.query(**params)["entries"]]

    def test_default_wrapped_order(self):
        self.assertEqual(self.sequences(), list(range(2, 10)))

    def test_full_pc_and_exclusive_upper(self):
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="8000100C"), [3, 5, 8])

    def test_newest_and_count_after_filter(self):
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="8000100C", newest=1, count=1), [8])
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="8000100C", count=1), [3])

    def test_combined_address_frame_filters(self):
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="8000100C",
                                        addr_lo="80001008", addr_hi="80001018",
                                        frame_lo=11, frame_hi=12), [5])

    def test_empty_ranges(self):
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="80001004"), [])
        self.assertEqual(self.sequences(pc_hi="0"), [])

    def test_dma_recorded_initiator_and_response_bounds(self):
        reply = self.query(pc_lo="80001008", pc_hi="8000100C")
        self.assertEqual(reply["pc_lo"], "0x80001008")
        self.assertEqual(reply["pc_hi"], "0x8000100C")
        self.assertEqual(reply["entries"][0]["dma_ch"], 2)
        self.assertEqual(reply["entries"][0]["seq"], 5)


if __name__ == "__main__":
    unittest.main()
