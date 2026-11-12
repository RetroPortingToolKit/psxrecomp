"""DMA6 (OTC) starts only when CHCR bit 28 is set.

Links the production dma.c and dma_gpu_ll.c into test_dma_otc_start.c
at -O0 and -O2; every other runtime seam is an abort() stub (fixture_link.py).
"""
import argparse
import tempfile
from pathlib import Path

from fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['dma.c', 'dma_gpu_ll.c', 'gpu_timeline.c'],
                          'test_dma_otc_start.c')
    print('PASS: DMA6 OTC starts only with CHCR bit 28 (O0/O2)')
