"""Exercise inline KSEG2 loads against the actual memory readers at O0/O2."""
import argparse
from pathlib import Path
import tempfile

from fixture_link import build_and_run


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as scratch:
        for opt in ("-O0", "-O2"):
            work = Path(scratch) / opt
            work.mkdir()
            build_and_run(args.cc, here, here.parent, opt, work,
                          ["memory.c", "crc32.c", "psx_ram_geometry.c"],
                          "test_kseg2_inline_load.c",
                          defines=("PSX_ENABLE_BLOCK_CYCLES=1",))
            print("PASS: inline KSEG2 loads", opt, flush=True)
