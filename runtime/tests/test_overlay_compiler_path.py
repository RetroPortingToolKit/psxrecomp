"""Compile the runtime resolver; invoke only its bundled compiler from another cwd."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root / 'src/main.cpp').read_text(encoding='utf-8')
    start = text.index('static std::filesystem::path resolve_overlay_compiler_path(')
    end = text.index('\n}\n', start) + 2
    block_start = text.index('        std::string tk_compiler;')
    block_end = text.index('        auto build_toolchain_cmd', block_start)
    metadata = text[block_start:block_end]
    hashes = (root / 'src/source_stateio_identity.c').read_text(encoding='utf-8')
    hs = hashes.index('int source_stateio_file_sha256(')
    hash_function = hashes[hs:hashes.index('\n}\n', hs) + 2]
    # Upstream keeps the hex formatting in a static helper beside the function.
    if 'static void sha256_hex(' in hashes:
        hx = hashes.index('static void sha256_hex(')
        hash_function = hashes[hx:hashes.index('\n}\n', hx) + 2] + '\n' + hash_function
    with tempfile.TemporaryDirectory(prefix='overlay compiler spaces ') as temp:
        base = Path(temp)
        toolkit = base / 'moved product/overlay_toolchain'
        bundled = toolkit / ('clang/bin/clang.exe' if os.name == 'nt' else 'clang/bin/clang')
        bundled.parent.mkdir(parents=True)
        unrelated = base / 'unrelated cwd'
        unrelated.mkdir()
        source = base / 'resolver.cpp'
        source.write_text('#include <filesystem>\n#include <string>\n#include <iostream>\n#include <fstream>\n#include <stdexcept>\n#include <type_traits>\n#include <cstdio>\n#ifdef _WIN32\n#include <windows.h>\n#endif\n#include "host_path.h"\n#include "psx_sha256.h"\n' + hash_function + '\n' + text[start:end] + '''
int main(int argc, char** argv) {
    const std::filesystem::path tk_dir(argv[1]);
    { std::ofstream config(tk_dir / "compiler.txt"); config << argv[2] << "\\n"; }
    auto init = [&]() {
''' + metadata + '''
    };
    static_assert(std::is_same_v<decltype(init()), void>, "overlay init must remain void");
    try { init(); } catch (const std::runtime_error&) { return 2; }
    const auto path = resolve_overlay_compiler_path(tk_dir, argv[2]);
    if (path.empty()) return 2;
    std::cout << path.string();
}
''')
        stub = base / 'compiler.cpp'
        stub.write_text('#include <cstdio>\nint main(){std::puts("bundled compiler invoked");}\n')
        probe = base / ('resolver.exe' if os.name == 'nt' else 'resolver')
        compiler = shutil.which(args.compiler) or args.compiler
        env = os.environ.copy()
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        for src, dst in ((source, probe), (stub, bundled)):
            subprocess.run([compiler, '-std=c++17', '-Werror=return-type', '-static',
                            '-I'+str(root/'include'), '-I'+str(root.parent/'recompiler/include'),
                            str(src), str(root/'src/psx_sha256.c'), '-o', str(dst)], check=True, env=env)
        relative = bundled.relative_to(toolkit).as_posix()
        for configured in (relative, str(bundled)):
            resolved = subprocess.check_output([str(probe), str(toolkit), configured], cwd=unrelated, text=True)
            assert Path(resolved) == bundled
            assert subprocess.check_output([resolved], cwd=unrelated, text=True).strip() == 'bundled compiler invoked'
        # A matching compiler on cwd/PATH must not satisfy a missing bundle.
        shutil.copy2(bundled, unrelated / bundled.name)
        bundled.unlink()
        env['PATH'] = str(unrelated) + os.pathsep + env['PATH']
        assert subprocess.run([str(probe), str(toolkit), relative], cwd=unrelated, env=env).returncode == 2
        assert subprocess.run([str(probe), str(toolkit), str(bundled)], cwd=unrelated, env=env).returncode == 2
    print('PASS: relative and legacy absolute compiler paths; spaces/unrelated cwd; missing bundle rejects host fallback')


if __name__ == '__main__':
    main()
