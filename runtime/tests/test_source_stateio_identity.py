"""Checkpoint identity dimensions, compiled at O0/O2 with -Werror."""
import argparse
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
            exe = root / ('stateio_identity' + opt + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([a.cc, '-std=c11', '-D_XOPEN_SOURCE=700', opt, '-Wall', '-Wextra', '-Werror',
                            '-I' + str(here.parent / 'include'),
                            str(here / 'test_source_stateio_identity.c'),
                            str(here.parent / 'src' / 'source_stateio_identity.c'),
                            str(here.parent / 'src' / 'psx_sha256.c'),
                            '-o', str(exe)], check=True)
            probe = root / ('probe' + opt + '.bin')
            base = {k: v for k, v in os.environ.items() if not k.startswith('PSX_')}

            def run(env, stored=None):
                probe.write_bytes(b'identity probe')
                argv = [str(exe), str(probe)] + ([stored] if stored else [])
                out = subprocess.run(argv, env=env, capture_output=True)
                assert out.returncode == 0, (opt, out.returncode, out.stdout, out.stderr)
                fields = {}
                for line in out.stdout.decode('utf-8', errors='replace').splitlines():
                    if '=' in line:
                        k, v = line.split('=', 1)
                        fields[k.strip()] = v.strip()
                return fields

            # Two distinct environments, so a constant-returning digest cannot pass.
            env_a = dict(base, PSX_TIMER1_MODEL='a')
            env_b = dict(base, PSX_TIMER1_MODEL='b')
            cfg_a, cfg_b = run(env_a)['config'], run(env_b)['config']
            assert len(cfg_a) == 64 and len(cfg_b) == 64, (opt, cfg_a, cfg_b)
            assert cfg_a != cfg_b, (opt, 'config digest did not react to PSX_TIMER1_MODEL')
            # A variable nobody listed still enters the identity.
            assert run(dict(env_a, PSX_FUTURE_MODEL='1'))['config'] != cfg_a, opt
            # Request variables do not: a resume run carries them, the capture did not.
            same = run(dict(env_a, PSX_TAS_RESUME_STATE='x.pst', PSX_TAS_SAVE_STATE_AT='5',
                            PSX_INPUT_ROUTE_FILE=str(probe)))['config']
            assert same == cfg_a, (opt, 'a request variable changed the config digest')
            # Non-PSX variables do not either.
            assert run(dict(env_a, UNRELATED='1'))['config'] == cfg_a, opt
            assert run(env_a, cfg_a)['accept'] == 'yes', opt
            assert run(env_b, cfg_a)['accept'] == 'refused', opt
            # The entire supported set is hashed; one more entry fails closed.
            maximum = dict(base, **{f'PSX_IDENTITY_{i:03d}': str(i) for i in range(512)})
            assert len(run(maximum)['config']) == 64, opt
            overflow = dict(maximum, PSX_IDENTITY_OVERFLOW='one-more')
            assert run(overflow, '--expect-overflow')['config'] == 'refused', opt
    print('PASS: checkpoint identity dimensions at O0/O2')


if __name__ == '__main__':
    main()
