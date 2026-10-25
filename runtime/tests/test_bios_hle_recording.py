"""Actual BIOS dispatch keeps diagnostic guest reads out of production."""
import argparse
from pathlib import Path
import tempfile
from fixture_link import build_and_run

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--cc',default='gcc')
    args=parser.parse_args();here=Path(__file__).resolve().parent
    for recording in (0,1):
        with tempfile.TemporaryDirectory(prefix='bios-recording-') as directory:
            work=Path(directory)
            (work/'fixture.c').write_text(f'#define EXPECT_RECORDING {recording}\n'+
                (here/'test_bios_hle_recording.c').read_text())
            build_and_run(args.cc,work,here.parent,'-O2',work,['bios_hle.c'],'fixture.c',
                defines=[] if recording else ['PSX_NO_DEBUG_TOOLS=1'])
    print('PASS: production dispatch records no forensic guest reads; debug repeats remain available')
