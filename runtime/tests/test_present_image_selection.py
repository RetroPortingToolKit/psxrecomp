"""Check the real build selector, including the reference/oracle target profile."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cmake', required=True)
    p.add_argument('--cc', required=True)
    p.add_argument('--ninja', required=True)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='present-selection-', dir=args.output) as scratch:
        scratch = Path(scratch)
        source = scratch/'src'
        source.mkdir()
        (source/'main.c').write_text('''#ifndef PSX_SHARED_PRESENT_IMAGES
#error Missing selected implementation
#endif
int main(void) { return PSX_SHARED_PRESENT_IMAGES != EXPECTED; }
''')
        (source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(present_binding C)
include("{root.as_posix()}/cmake/psx_present_images.cmake")
add_executable(probe main.c)
set_property(TARGET probe PROPERTY PSX_EXECUTION_PROFILE "${{PROFILE}}")
psxrecomp_present_images(probe)
target_compile_definitions(probe PRIVATE EXPECTED=${{EXPECTED}})
''')
        for label, profile, option, expected in [
            ('default', 'ENHANCED', None, 1), ('on', 'ENHANCED', 'ON', 1),
            ('off', 'ENHANCED', 'OFF', 0), ('reference', 'REFERENCE', None, 0),
            ('reference-on', 'REFERENCE', 'ON', 0)]:
            output = scratch/label
            command = [args.cmake, '-S', str(source), '-B', str(output), '-G', 'Ninja',
                       '-DCMAKE_C_COMPILER='+args.cc, '-DCMAKE_MAKE_PROGRAM='+args.ninja,
                       '-DPROFILE='+profile, '-DEXPECTED='+str(expected)]
            if option:
                command.append('-DPSX_SHARED_PRESENT_IMAGES='+option)
            subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            subprocess.run([args.cmake, '--build', str(output), '--parallel', '1'], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            exe = output/('probe.exe' if __import__('os').name == 'nt' else 'probe')
            subprocess.run([str(exe)], check=True)
            print(label, 'passed')


if __name__ == '__main__':
    main()
