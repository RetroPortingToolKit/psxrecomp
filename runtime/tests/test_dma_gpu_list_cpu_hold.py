"""The GPU linked-list DMA kick holds the CPU until the walk ends.

Links the production dma.c and dma_gpu_ll.c into test_dma_gpu_list_cpu_hold.c
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
                          'test_dma_gpu_list_cpu_hold.c')
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['dma.c', 'dma_gpu_ll.c', 'gpu_timeline.c'],
                          'test_dma_gpu_native_submission.c',
                          defines=('PSX_NATIVE_FRAME_SUBMISSION=1',))
    print('PASS: the GPU linked-list kick holds the CPU until the walk ends (O0/O2)')
