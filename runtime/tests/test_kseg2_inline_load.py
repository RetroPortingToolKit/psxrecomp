"""Exercise inline KSEG2 loads against the actual memory readers at O0/O2."""
import argparse
from pathlib import Path
import tempfile

from fixture_link import build_and_run


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--headroom-disabled",action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as scratch:
        for opt in ("-O0", "-O2"):
            work = Path(scratch) / opt
            work.mkdir()
            build_and_run(args.cc, here, here.parent, opt, work,
                          ["memory.c", "crc32.c", "psx_ram_geometry.c"],
                          "test_kseg2_inline_load.c",
                          defines=("PSX_ENABLE_BLOCK_CYCLES=1", "PSX_NO_DEBUG_TOOLS=1") +
                          (("PSX_DISABLE_HEADROOM_COUNTERS=1",) if args.headroom_disabled else ()))
            print("PASS: inline KSEG2 loads", opt, flush=True)
