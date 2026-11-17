"""Execute the actual shipping ring adapter with controlled GPU image callbacks."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    runtime=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='psx-display-history-') as tmp:
        work=Path(tmp)
        for native in (0,1):
            for opt in ('-O0','-O2'):
                exe=work/('history.exe' if os.name=='nt' else 'history')
                subprocess.run([args.cc,'-std=c11',opt,'-Wall','-Wextra',
                    f'-DPSX_GPU_DISPLAY_HISTORY={native}','-I',str(work),
                    '-I',str(runtime/'include'),'-I',str(runtime/'src'),str(runtime/'tests/test_display_history.c'),
                    '-o',str(exe)],check=True)
                env={k:v for k,v in os.environ.items() if not k.upper().startswith('PSX_')}
                subprocess.run([str(exe)],env=env,check=True,timeout=30)
    print('PASS: production display-history adapter reference/native O0/O2')


if __name__=='__main__': main()
