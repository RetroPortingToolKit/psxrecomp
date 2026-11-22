"""Execute production Vulkan submission/resource functions with legal image orders."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'static\s+(?:int|void)\s+' + name + r'\([^;]*?\)\s*\{', source)
    if not match:
        return None
    opening = source.index('{', match.start())
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', required=True)
    parser.add_argument('--vulkan-include', required=True)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--fixture', type=Path, default=Path(__file__).with_suffix('.c'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (args.source or root / 'runtime/src/gpu_vk_renderer.c').read_text(encoding='utf-8')
    image_owned = function(source, 'create_present_semaphores') is not None
    array = 's_sem_present' if image_owned else 's_sem_render'
    declaration = re.search(r'static\s+VkSemaphore\s+' + array + r'\[[^]]+\];', source).group(0)
    frames = re.search(r'#define VK_FRAMES\s+\d+', source).group(0)
    declarations = frames + '\n' + declaration + '\n'
    declarations += '#define PRESENT_ARRAY ' + array + '\n'
    declarations += '#define HAVE_IMAGE_SEMAPHORES ' + str(int(image_owned)) + '\n'
    functions = []
    for name in ('destroy_present_semaphores', 'create_present_semaphores', 'submit_present'):
        body = function(source, name)
        if body:
            functions.append(body)
    with tempfile.TemporaryDirectory(prefix='psx-present-lifetime-') as temporary:
        work = Path(temporary)
        (work / 'present_declarations.inc').write_text(declarations, encoding='utf-8')
        (work / 'present_functions.inc').write_text('\n'.join(functions), encoding='utf-8')
        executable = work / 'present.exe'
        subprocess.run([args.cc, '-std=c11', '-O2', '-UNDEBUG',
                        '-Werror=incompatible-pointer-types',
                        '-I', str(work), '-I', args.vulkan_include,
                        '-I', str(root / 'runtime/include'), str(args.fixture),
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
