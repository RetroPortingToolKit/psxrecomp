"""--static must reach the end of main() and exit cleanly.

Regression for 3a174fab: reconcile_empty_primary_scans() was called at the
tail of main() with cache_dir, a local bound only on the DLL path. Every
--static run then died with UnboundLocalError after its output was already
written, exiting 1, and axis_b_loop.sh (which accepts only 0 or 2) aborted
before the runtime rebuild.

An empty capture list is the smallest input that walks the whole of main()
in static mode, so no game assets are involved. The recompiler binary is
still required: compile_overlays asks it for --codegen-hash as a staleness
check before doing anything else. CTest passes it the same way the other
overlay codegen tests get it.

Usage: test_compile_overlays_static_tail.py --recompiler <psxrecomp-game>
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
RECOMPILER = None


class StaticTailTest(unittest.TestCase):
    def test_static_run_with_no_captures_exits_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            caps = tmp / "captures.json"
            caps.write_text(json.dumps([]), encoding="utf-8")
            toml = tmp / "game.toml"
            toml.write_text('[game]\nid = "TEST-00000"\nname = "static tail"\n',
                            encoding="utf-8")
            out = tmp / "out"
            proc = subprocess.run(
                [sys.executable, str(TOOLS / "compile_overlays.py"),
                 "--static",
                 "--captures", str(caps),
                 "--out-dir", str(out),
                 "--game-toml", str(toml),
                 "--recompiler", RECOMPILER,
                 "--runtime-include", str(ROOT / "runtime" / "include")],
                capture_output=True, text=True, timeout=120)
            detail = proc.stdout[-1500:] + proc.stderr[-1500:]
            # The contract is only that main() runs to its end. With nothing
            # captured there is legitimately no overlays_static.c to write,
            # so the output file is not part of it.
            self.assertNotIn("UnboundLocalError", detail, detail)
            self.assertIn("Done.", detail, detail)
            self.assertEqual(proc.returncode, 0, detail)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", required=True,
                    help="psxrecomp-game binary (compile_overlays calls it "
                         "for --codegen-hash)")
    args, rest = ap.parse_known_args()
    if not Path(args.recompiler).is_file():
        sys.exit("no recompiler at %s" % args.recompiler)
    RECOMPILER = args.recompiler
    unittest.main(argv=[sys.argv[0]] + rest)
