"""Exercise actual DMA/MDEC modules in maintained word and native job builds."""
import argparse
from pathlib import Path
import tempfile
from fixture_link import build_and_run

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    here=Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as scratch:
        for dma in ('dma.c','dma_media_jobs.c'):
            for opt in ('-O0','-O2'):
                work=Path(scratch)/(dma+opt)
                work.mkdir()
                build_and_run(args.cc,here,here.parent,opt,work,
                              [dma,'mdec.c','dma_gpu_ll.c','gpu_timeline.c'],
                              'test_dma_mdec_jobs.c')
                print('PASS',dma,opt,flush=True)
