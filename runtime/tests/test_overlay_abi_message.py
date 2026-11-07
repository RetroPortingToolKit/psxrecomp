"""Execute the real bounded logger and both platform ABI diagnostics."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'src/overlay_loader.c')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8-sig')
    size = re.search(r'static char\s+s_last_msg\[(\d+)\]', source)
    if size is None:
        raise ValueError('bounded loader message buffer not found')
    signature = 'static void loader_log(const char *fmt, ...) {'
    start = source.index(signature)
    logger = source[start:source.index('\n}', start) + 2]
    calls = re.findall(r'loader_log\("ABI/flavor mismatch.*?\);', source, re.S)
    if len(calls) != 2:
        raise ValueError('expected the Windows and POSIX ABI mismatch calls')
    buffer_size = int(size[1])
    program = '''#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define PSX_OVERLAY_ABI_TAG 0x40506
static char s_last_msg[BUFFER_SIZE];
LOGGER
static unsigned failures, cases;
static void check_message(const char *path, int is_long) {
    cases++;
    if (!strstr(s_last_msg, "dll=0x10203")) {
        fprintf(stderr, "case %u: DLL tag lost: %s\\n", cases, s_last_msg);
        failures++;
    }
    if (!strstr(s_last_msg, "runtime=0x40506")) {
        fprintf(stderr, "case %u: runtime tag lost: %s\\n", cases, s_last_msg);
        failures++;
    }
    if (!is_long && !strstr(s_last_msg, path)) {
        fprintf(stderr, "case %u: short path lost\\n", cases);
        failures++;
    }
}
int main(void) {
    int abi = 0x10203;
    char dll_path[BUFFER_SIZE * 4];
    for (int is_long = 0; is_long < 2; ++is_long) {
        memset(dll_path, 'x', sizeof dll_path - 1);
        dll_path[sizeof dll_path - 1] = 0;
        if (!is_long) strcpy(dll_path, "cache/example-overlay");
        CALL_WINDOWS
        check_message(dll_path, is_long);
        CALL_POSIX
        check_message(dll_path, is_long);
    }
    printf("%u ABI message cases, %u failures\\n", cases, failures);
    return failures ? 1 : 0;
}
'''.replace('BUFFER_SIZE', str(buffer_size)).replace('LOGGER', logger)
    program = program.replace('CALL_WINDOWS', calls[0]).replace('CALL_POSIX', calls[1])
    options = dict(stdin=subprocess.DEVNULL, capture_output=True, text=True,
                   encoding='utf-8', errors='replace', timeout=90)
    if os.name == 'nt':
        options['creationflags'] = (subprocess.CREATE_NO_WINDOW |
                                    subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        unit = root / 'abi_message.c'
        executable = root / ('abi_message.exe' if os.name == 'nt' else 'abi_message')
        unit.write_text(program, encoding='utf-8')
        compile_result = subprocess.run(
            [args.cc, '-std=c99', '-O2', str(unit), '-o', str(executable)], **options)
        if compile_result.returncode:
            raise RuntimeError(compile_result.stdout + compile_result.stderr)
        result = subprocess.run([str(executable)], **options)
        print(result.stdout, end='')
        if result.stderr:
            print(result.stderr, end='')
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
