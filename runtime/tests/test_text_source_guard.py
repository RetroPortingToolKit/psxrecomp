"""Refuse native text dispatch where the loaded executable differs from the
generated-from image, through the real memory.c guard, at O0 and O2."""
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
                          ["memory.c", "text_source_guard.c", "crc32.c",
                           "psx_ram_geometry.c"],
                          "test_text_source_guard.c",
                          defines=("PSX_ENABLE_BLOCK_CYCLES=1",))
            print("PASS: text source guard", opt, flush=True)
