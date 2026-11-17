"""Production sample scheduler latch/rearm and exact block-partition PCM."""
import argparse
import tempfile
from pathlib import Path
from fixture_link import build_and_run

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as work:
        for mode in (0, 1):
            for opt in ("-O0", "-O2"):
                build_and_run(args.cc, here, here.parent, opt, Path(work),
                              ["interrupts_block_audio.c" if mode else "interrupts.c"],
                              "test_spu_guest_clock.c",
                              defines=("psx_irq_raise=fixture_unused_irq_raise",),
                              fixture_defines=("PSX_TEST_SCHEDULER=1", "PSX_BLOCK_AUDIO=" + str(mode)))
    print("PASS: reference/native scheduler latch/rearm and exact nonzero PCM (O0/O2)")
