"""Run owned-memory and host GPU copy/rollback checks with abort-only seams."""
import argparse
import tempfile
from pathlib import Path
from fixture_link import build_and_run

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        work = Path(root)
        # The native CMake fixture includes memory.c directly. Preserve that
        # fixture while making its source include work outside runtime/src.
        source = (here / "test_mod_memory_snapshot.c").read_text()
        source = ('#pragma GCC diagnostic ignored "-Wcomment"\n'
                  '#pragma GCC diagnostic ignored "-Wunused-function"\n') + source.replace(
            '#include "memory.c"', '#include "' + (here.parent / "src/memory.c").as_posix() + '"')
        (work / "snapshot.c").write_text(source)
        for opt in ("-O0", "-O2"):
            build_and_run(args.cc, work, here.parent, opt, work,
                          ["psx_ram_geometry.c"], "snapshot.c")
    print("PASS: owned-memory snapshots and host GPU copy rollback (O0/O2)")
