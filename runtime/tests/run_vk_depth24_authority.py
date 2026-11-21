"""Exercise the production Vulkan RGB888 ownership path on a hidden device."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cc', 'sdl-include', 'sdl-library', 'vulkan-include', 'glslc', 'output'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--renderer-source', type=Path,
                        help='exercise a frozen renderer source for a before/after check')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    records = []

    def run(command):
        command = [str(x) for x in command]
        result = subprocess.run(command, cwd=out, capture_output=True, text=True,
                                encoding='utf-8', errors='replace')
        records.append(dict(argv=command, exit=result.returncode,
                            stdout=result.stdout, stderr=result.stderr))
        (out / 'receipt.json').write_text(json.dumps(records, indent=2) + '\n')
        print(result.stdout, result.stderr, flush=True)
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command)

    # Use the actual movie scanout decoder as well as the actual renderer.
    source = (root / 'runtime/src/gpu.c').read_text(encoding='utf-8')
    start = source.index('void gpu_display_pixel_rgb_in(')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    (out / 'vk_depth24_scanout.inc').write_text(source[start:end], encoding='utf-8')
    shaders = root / 'runtime/shaders'
    run([sys.executable, root / 'tools/embed_spirv.py', '--glslc', args.glslc,
         '--out', out / 'vk_shaders_spv.h',
         *sorted(p for p in shaders.iterdir() if p.suffix in ('.vert', '.frag', '.comp'))])
    flags = ['-std=gnu11', '-O1', '-DPSX_SDL3=1', '-DPSX_NO_DEBUG_TOOLS=1',
             '-ffunction-sections', '-fdata-sections', '-I', root / 'runtime/include',
             '-I', out, '-I', root / 'runtime/src', '-I', args.vulkan_include]
    if args.renderer_source:
        flags += ['-DPSX_VK_RENDERER_SOURCE="' + args.renderer_source.resolve().as_posix() + '"']
    for include in args.sdl_include.split(';'):
        if include:
            flags += ['-I', include]
    libraries = ['-lm']
    if platform.system() == 'Windows':
        flags += ['-flto', '-fwhole-program']
        libraries += ['-static', '-Wl,--gc-sections']
        libraries += ['-l' + name for name in (
            'kernel32', 'user32', 'gdi32', 'winmm', 'imm32', 'ole32', 'oleaut32',
            'version', 'uuid', 'advapi32', 'setupapi', 'shell32', 'dinput8',
            'd2d1', 'dwrite', 'windowscodecs', 'iphlpapi', 'dbghelp', 'ws2_32')]
    else:
        libraries += ['-Wl,--gc-sections', '-ldl', '-lpthread']
    executable = out / ('depth24.exe' if os.name == 'nt' else 'depth24')
    run([args.cc, *flags, args.fixture or root / 'runtime/tests/test_vk_depth24_authority.c',
         root / 'runtime/src/gpu_sw_renderer.c', args.sdl_library, '-o', executable,
         *libraries])
    run([executable])
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
