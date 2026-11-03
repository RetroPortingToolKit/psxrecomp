"""Execute the production forensic ring and scanout decoder on authored VRAM.

PNG encoding, GL readback and RPC transport are boundary stubs. No game inputs.
"""
import argparse
import os
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--work-dir', required=True, type=Path)
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1]
    server = (runtime / 'src/debug_server.c').read_text(encoding='utf-8')
    gpu = (runtime / 'src/gpu.c').read_text(encoding='utf-8')
    declarations = server[server.index('#define DISP_RING_CAP'):server.index('static void disp_ring_capture(void)')]
    production = declarations + function(server, 'static void disp_ring_capture(void)')
    production += function(server, 'static void handle_display_ring_get(int id,')
    decoder = function(gpu, 'static void gpu_rgb555_to_rgb888(')
    decoder += function(gpu, 'void gpu_display_pixel_rgb_in(')
    fixture = (runtime / 'tests/display_ring_fixture.c').read_text(encoding='utf-8')
    source = fixture.replace('/* PRODUCTION_DECODER */', decoder).replace('/* PRODUCTION_RING */', production)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    unit = work / 'display_ring.c'
    unit.write_text(source, encoding='utf-8')
    env = os.environ.copy()
    env.pop('PSX_DISPLAY_RING', None)
    env['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get('PATH', '')
    for optimization in ('-O0', '-O2'):
        binary = work / ('ring-' + optimization[1:] + ('.exe' if os.name == 'nt' else ''))
        for command in ([args.cc, optimization, '-std=c11', '-I', str(runtime / 'include'), str(unit), str(runtime / 'src/color_lut.c'), '-lm', '-o', str(binary)], [str(binary)]):
            result = subprocess.run(command, cwd=work, env=env, text=True, encoding='utf-8', errors='replace', capture_output=True)
            print(result.stdout, end='', flush=True)
            print(result.stderr, end='', flush=True)
            if result.returncode:
                raise SystemExit(result.returncode)
    print('DISPLAY_RING_OPTIMIZATIONS_PASS=2')


if __name__ == '__main__':
    main()
