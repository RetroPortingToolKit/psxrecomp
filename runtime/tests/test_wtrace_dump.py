"""Execute the production trace-query handler against a wrapped fixture ring.

Compile only the handler and its real JSON helpers, replacing the transport
with stdout. This needs a C compiler, but no game image, SDL or live server.
Exits 77 (ctest SKIP_RETURN_CODE) when no C compiler is available.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

SKIP_EXIT = 77


def matching_brace(source, open_index):
    """Index of the brace closing the one at open_index (skips strings/comments)."""
    depth, i, n = 0, open_index, len(source)
    while i < n:
        c = source[i]
        if source.startswith("/*", i):
            i = source.index("*/", i + 2) + 1
        elif source.startswith("//", i):
            i = source.index("\n", i)
        elif c in "\"'":
            i += 1
            while source[i] != c:
                i += 2 if source[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise AssertionError("unbalanced braces")


def find_compiler():
    """argv prefix for the C compiler named by CC (default cc), or None."""
    setting = os.environ.get("CC", "cc")
    if shutil.which(setting):
        return [setting]
    parts = shlex.split(setting, posix=os.name != "nt")
    return parts if parts and shutil.which(parts[0]) else None


class WriteTraceQuery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = find_compiler()
        if compiler is None:
            raise unittest.SkipTest("no C compiler (set CC)")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        source = (Path(__file__).parents[1] / "src/debug_server.c").read_text(encoding="utf-8")

        def function(name):
            match = re.search(r"static [^;{}]+\b" + name + r"\([^;{}]*\)\s*\{", source)
            if not match:
                raise AssertionError(f"production function missing: {name}")
            return source[match.start():matching_brace(source, match.end() - 1) + 1]

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
typedef struct { uint32_t pc; uint32_t gpr[32]; } CPUState;
static CPUState *debug_cpu_ptr = NULL;
static uint32_t s_frame_count = 0;
PSX_THREAD_LOCAL uint32_t g_debug_current_func_addr = 0;
PSX_THREAD_LOCAL uint32_t g_debug_last_store_pc = 0;
PSX_THREAD_LOCAL int g_dma_exec_depth = 0, g_dma_cur_ch = -1;
PSX_THREAD_LOCAL uint32_t g_dma_initiator_pc = 0;
static void debug_server_send_line(const char *line) { puts(line); }
static void send_err(int id, const char *error) { (void)id; (void)error; abort(); }
"""
        for name in ("json_get_str", "json_get_int", "hex_to_u32", "wtrace_fill_entry",
                     "handle_wtrace_dump"):
            harness += function(name) + "\n"
        # seq 7 is a DMA entry with an unknown (0) initiator PC.
        harness += """int main(int argc, char **argv) {
    if (argc > 4 && !strcmp(argv[1], "fill")) {   /* fill <dma_depth> <initiator> <cpu_store_pc> */
        WriteTraceEntry e; memset(&e, 0, sizeof e);
        g_dma_exec_depth = atoi(argv[2]); g_dma_cur_ch = 2;
        g_dma_initiator_pc = (uint32_t)strtoul(argv[3], 0, 16);
        g_debug_last_store_pc = (uint32_t)strtoul(argv[4], 0, 16);
        wtrace_fill_entry(&e, 1, 0x1000, 0, 1, 4);
        printf("{\\"pc\\":%u,\\"dma_ch\\":%d}\\n", e.pc, e.dma_ch);
        return 0;
    }
    const uint32_t pcs[8]={0x80001000,0x80001004,0xA0001000,0x80001008,
                           0x8000100C,0x00000000,0x80001004,0x00001004};
    for (int seq=2; seq<10; ++seq) {
        WriteTraceEntry *e=&ring[seq%WRITE_TRACE_CAP];
        e->seq=seq; e->pc=pcs[seq-2]; e->addr=0x1000+(seq-2)*4;
        e->frame=10+(seq-2)/2; e->width=4; e->dma_ch=seq==5?2:(seq==7?3:-1);
    }
    handle_wtrace_dump(7,argc>1?argv[1]:"{}");
    return 0;
}
"""
        path = Path(cls.temp.name)
        (path / "query.c").write_text(harness, encoding="utf-8", newline="\n")
        cls.exe = path / ("query.exe" if os.name == "nt" else "query")
        if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
            out = ["/nologo", "/Fe:" + str(cls.exe), "/Fo:" + str(path) + os.sep]
        else:
            out = ["-o", str(cls.exe)]
        subprocess.run([*compiler, str(path / "query.c"), *out], check=True, cwd=path)

    def query(self, **params):
        result = subprocess.check_output([str(self.exe), json.dumps(params)], text=True)
        reply = json.loads(result)
        self.assertEqual(reply["emitted"], len(reply["entries"]))
        return reply

    def sequences(self, **params):
        return [e["seq"] for e in self.query(**params)["entries"]]

    def fill(self, depth, initiator, store_pc):
        out = subprocess.check_output(
            [str(self.exe), "fill", str(depth), initiator, store_pc], text=True)
        return json.loads(out)

    def test_fill_entry_producer_pc(self):
        self.assertEqual(self.fill(0, "0", "80001234"), {"pc": 0x80001234, "dma_ch": -1})
        self.assertEqual(self.fill(1, "80005000", "80001234"), {"pc": 0x80005000, "dma_ch": 2})
        # Unknown initiator: the DMA entry must not inherit the stale CPU store PC.
        self.assertEqual(self.fill(1, "0", "80001234"), {"pc": 0, "dma_ch": 2})

    def test_default_wrapped_order(self):
        self.assertEqual(self.sequences(), list(range(2, 10)))

    def test_pc_range_exclusive_upper(self):
        # seq 9 has KUSEG pc 0x1004, which aliases 0x80001004.
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="8000100C"), [3, 5, 8, 9])

    def test_segments_alias(self):
        self.assertEqual(self.sequences(pc_lo="A0001000", pc_hi="A0001004"), [2, 4])
        self.assertEqual(self.sequences(pc_lo="00001000", pc_hi="00001004"), [2, 4])
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="80001008"), [3, 8, 9])

    def test_unknown_dma_initiator_never_matches_pc_filter(self):
        self.assertIn(7, self.sequences())
        self.assertNotIn(7, self.sequences(pc_lo="0", pc_hi="FFFFFFFF"))
        self.assertEqual(self.sequences(pc_hi="80001000"), [])

    def test_newest_and_count_after_filter(self):
        self.assertEqual(self.sequences(pc_lo="80001004", pc_hi="8000100C", newest=1, count=1), [9])
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
    if find_compiler() is None:
        print("SKIP: no C compiler (set CC)")
        sys.exit(SKIP_EXIT)
    unittest.main()
