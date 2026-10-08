"""Relative paths persisted in settings.toml and the bios.cfg / disc.cfg
sidecars resolve from the executable directory, not the working directory.

Compiles the real adapters and sidecar writer/reader from main.cpp, plus
relative_to_folder from config_loader.cpp, into a probe and runs it from an
unrelated working directory. Checks save/reopen as well as path consumers.
No game data is used.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

# Every place main.cpp reads a persisted path must anchor it on the exe
# directory. Each line below must appear in main.cpp exactly as written.
CALL_SITES = [
    'resolved_disc = resolve_persisted_disc_path(us.disc_path, exe_dir_from_argv(argv[0]))',
    'cached = resolve_persisted_disc_path(cached, exe_dir_from_argv(argv0))',
    ': anchor_on_exe_dir(argv0, std::filesystem::path(line));',
    'settings_bios_storage = anchor_on_exe_dir(argv[0], us.bios_path).string();',
    'memcard_dir   = anchor_on_exe_dir(argv[0], us.memcard_dir);',
    'memcard1_path = anchor_on_exe_dir(argv[0], us.memcard1_path);',
    'memcard2_path = anchor_on_exe_dir(argv[0], us.memcard2_path);',
    'memcard1_path = anchor_on_exe_dir(argv[0], seed.memcard1_path);',
    'memcard2_path = anchor_on_exe_dir(argv[0], seed.memcard2_path);',
]


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True,
                          encoding='utf-8', errors='replace', **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root / 'src/main.cpp').read_text(encoding='utf-8')
    for site in CALL_SITES:
        assert site in text, 'main.cpp no longer anchors: ' + site
    adapters = []
    for name in ('normalize_disc_path_for_launch', 'resolve_persisted_disc_path',
                 'anchor_on_exe_dir'):
        start = text.index('static std::filesystem::path ' + name + '(')
        adapters.append(text[start:text.index('\n}\n', start) + 2])
    config = (root.parent / 'recompiler/src/config_loader.cpp').read_text(encoding='utf-8')
    start = config.index('fs::path relative_to_folder(')
    relative_adapter = config[start:config.index('\n}\n', start) + 2]
    for declaration in ('static std::filesystem::path read_cached_path(',
                        'static void write_cached_path('):
        start = text.index(declaration)
        adapters.append(text[start:text.index('\n}\n', start) + 2])
    with tempfile.TemporaryDirectory(prefix='persisted paths spaces ') as tmp:
        base = Path(tmp)
        product, cwd = base / 'moved product', base / 'unrelated cwd'
        (product / 'inputs').mkdir(parents=True)
        (cwd / 'inputs').mkdir(parents=True)
        relative = 'inputs/Some Game [Disc 1] (v1.1).chd'
        (product / relative).write_bytes(b'authored placeholder')
        (cwd / relative).write_bytes(b'wrong cwd decoy')
        source = base / 'probe.cpp'
        # The probe's exe_dir_from_argv() stands in for the real one: the
        # "argv0" it receives is the product directory itself.
        source.write_text('''#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "host_path.h"
#include "disc_path.h"
namespace fs = std::filesystem;
namespace PSXRecompV4 {
''' + relative_adapter + '''
}
static std::filesystem::path exe_dir_from_argv(const char* dir) {
    return std::filesystem::path(dir);
}
static std::filesystem::path sidecar_cfg_path(const char* dir, const char* filename) {
    return exe_dir_from_argv(dir) / filename;
}
''' + '\n'.join(adapters) + '''
int main(int argc, char** argv) {
    (void)argc;
    const std::string mode = argv[1];
    const std::filesystem::path path = argv[2];
    std::filesystem::path resolved =
        mode == "persisted" ? resolve_persisted_disc_path(path, argv[3])
      : mode == "anchor"    ? anchor_on_exe_dir(argv[3], path)
      :                       normalize_disc_path_for_launch(path);
    if (mode == "sidecar") {
        write_cached_path(argv[3], "disc.cfg", path);
        resolved = read_cached_path(argv[3], "disc.cfg");
    }
    // libstdc++'s generic_string() collapses a UNC "\\\\server" root to "/server";
    // print the native spelling with forward slashes so both C++ libraries agree.
    std::string out = resolved.string();
    for (char& c : out) if (c == '\\\\') c = '/';
    std::cout << out;
}
''', encoding='utf-8')
        compiler = shutil.which(args.compiler) or args.compiler
        env = os.environ.copy()
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        exe = base / ('probe.exe' if os.name == 'nt' else 'probe')
        subprocess.run([compiler, '-std=c++17', '-Werror=return-type', '-I'+str(root/'include'),
                        '-I'+str(root.parent/'recompiler/include'), str(source),
                        str(root/'src/disc_path.cpp'), str(root/'src/cue_sheet.cpp'), '-o', str(exe)],
                       check=True, env=env)

        def probe(mode, path):
            result = run([str(exe), mode, str(path), str(product)], cwd=cwd, env=env)
            assert result.returncode == 0, (mode, path, result.stderr)
            return result.stdout

        def check(mode, path, expected):
            out = probe(mode, path)
            assert out == expected, (mode, path, out, expected)

        def check_same(mode, path, expected):
            out = probe(mode, path)
            assert Path(out).resolve() == Path(expected).resolve(), (mode, path, out, expected)

        # [disc] path and disc.cfg
        check('persisted', relative, (product/relative).as_posix())
        check('persisted', product/relative, (product/relative).as_posix())
        check('persisted', '', '')
        check('persisted', 'inputs/missing.chd', (product/'inputs/missing.chd').as_posix())
        check('cli', relative, (cwd/relative).as_posix())
        check('sidecar', product/relative, (product/relative).as_posix())
        check('sidecar', '', '')
        if os.name == 'nt':
            for spelling in (r'\\server\share\Some Game [Disc 1].chd',
                             '//server/share/Some Game [Disc 1].chd'):
                check('persisted', spelling, '//server/share/Some Game [Disc 1].chd')
                check('sidecar', spelling, '//server/share/Some Game [Disc 1].chd')
        # [bios] path, bios.cfg and [memcard] dir/card1/card2
        check_same('anchor', '.', product)
        check_same('anchor', 'cards/card1.mcd', product/'cards/card1.mcd')
        check_same('anchor', 'bios/SCPH1001.BIN', product/'bios/SCPH1001.BIN')
        check('anchor', product/'cards', (product/'cards').as_posix())
        check('anchor', '', '')
        print('persisted paths: disc consumers and sidecar save/reopen '
              '(relative, absolute, empty, missing, CLI cwd, Windows UNC) '
              'and BIOS/memory card (".", relative, absolute, empty) passed')


if __name__ == '__main__':
    main()
