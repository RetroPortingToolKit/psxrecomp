"""TAS checkpoint manifest + identity gate, compiled at O0/O2 with -Werror."""
import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--cc', default='gcc')
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for opt in ['-O0', '-O2']:
            exe = root / ('tasstateio' + opt + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([a.cc, '-std=c11', '-D_XOPEN_SOURCE=700', opt, '-Wall', '-Wextra', '-Werror',
                            '-I' + str(here.parent / 'include'),
                            str(here / 'test_source_tas_stateio.c'), '-o', str(exe)],
                           check=True)
            workdir = root / ('run' + opt)
            workdir.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith('PSX_')}
            result = subprocess.run([str(exe), str(workdir)], env=env,
                                    capture_output=True, timeout=30)
            assert result.returncode == 0, (opt, result.returncode,
                                            result.stdout, result.stderr)
            assert (workdir / 'tas-state-000300.pst.json').exists(), opt
            manifest=json.loads((workdir/'tas-state-000300.pst.json').read_text())
            assert manifest['input_consumed']==301 and manifest['frame']==300
            assert Path(manifest['state_path'])==workdir/'tas-state-000300.pst'
    print('PASS: TAS checkpoint manifest round-trip and identity gate at O0/O2')

if __name__ == '__main__':
    main()
