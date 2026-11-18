"""Exercise the actual enclosing startup lambda with authored managed-store files."""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--verify-toolkit', type=Path, help='Read-only cross-check of a backend-produced managed toolkit')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    text = (root/'src/main.cpp').read_text(encoding='utf-8')
    start = text.index('static std::filesystem::path resolve_overlay_compiler_path(')
    resolver = text[start:text.index('\n}\n', start)+2]
    start = text.index('        std::string tk_compiler;')
    metadata = text[start:text.index('        auto build_toolchain_cmd', start)]
    hashes = (root/'src/source_stateio_identity.c').read_text(encoding='utf-8')
    start = hashes.index('int source_stateio_file_sha256(')
    hash_function = hashes[start:hashes.index('\n}\n',start)+2]
    # Upstream keeps the hex formatting in a static helper beside the function.
    if 'static void sha256_hex(' in hashes:
        hx = hashes.index('static void sha256_hex(')
        hash_function = hashes[hx:hashes.index('\n}\n', hx) + 2] + '\n' + hash_function
    with tempfile.TemporaryDirectory(prefix='shared overlay spaces ') as temp:
        base=Path(temp)
        toolkit=base/'moved game/overlay_toolchain'
        toolkit.mkdir(parents=True)
        cwd=base/'unrelated cwd'
        cwd.mkdir()
        staged=base/'new store'
        (staged/'bin').mkdir(parents=True)
        (staged/'lib').mkdir()
        support=staged/'lib/support.dll'
        support.write_bytes(b'authored supporting file')
        compiler=shutil.which(args.compiler) or args.compiler
        env=os.environ.copy()
        env['PATH']=str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
        stub=base/'stub.cpp'
        stub.write_text('#include <cstdio>\nint main(){puts("shared compiler invoked");}\n')
        bundled=staged/'bin/clang.exe'
        subprocess.run([compiler,'-static',str(stub),'-o',str(bundled)],env=env,check=True)
        manifest=''.join(f'{digest(p)}  {p.relative_to(staged).as_posix()}\n' for p in (bundled,support))
        content=hashlib.sha256(manifest.encode()).hexdigest()
        store=base/('overlay-clang-'+content)
        staged.rename(store)
        (store/'SHA256SUMS.txt').write_bytes(manifest.encode())
        bundled=store/'bin/clang.exe'
        (toolkit/'compiler.txt').write_text(str(bundled)+'\n')
        marker=toolkit/'compiler-store.sha256'
        marker.write_bytes((content+'\n').encode())
        probe=base/'probe.exe'
        src=base/'probe.cpp'
        src.write_text('''#include <filesystem>
#include <string>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <cstdio>
#include <windows.h>
#include "host_path.h"
#include "psx_sha256.h"
''' + hash_function+'\n'+resolver+'''
int main(int argc,char** argv) {
    std::filesystem::path tk_dir(argv[1]);
    auto init=[&]() {
''' + metadata + '''
        std::cout << (tk_compiler.empty() ? "HOST_FALLBACK" : tk_compiler);
    };
    static_assert(std::is_same_v<decltype(init()),void>);
    try { init(); } catch(const std::exception& e) { std::cerr << e.what(); return 2; }
    if (argc > 2) { std::cout << std::endl; std::cin.get(); }
}
''',encoding='utf-8')
        subprocess.run([compiler,'-std=c++17','-Werror=return-type','-static','-I'+str(root/'include'),
                        '-I'+str(root.parent/'recompiler/include'),str(src),str(root/'src/psx_sha256.c'),'-o',str(probe)],env=env,check=True)
        if args.verify_toolkit:
            checked=subprocess.run([str(probe),str(args.verify_toolkit.resolve())],cwd=cwd,env=env,text=True,capture_output=True,check=True)
            assert Path(checked.stdout)==Path((args.verify_toolkit/'compiler.txt').read_text().strip())
            print('Backend manifest accepted by actual runtime resolver:',checked.stdout)
        # A host compiler decoy cannot rescue a managed store failure.
        shutil.copy2(bundled,cwd/'gcc.exe')
        env['PATH']=str(cwd)+os.pathsep+env['PATH']
        def run(ok, offender=None):
            r=subprocess.run([str(probe),str(toolkit)],cwd=cwd,env=env,text=True,capture_output=True)
            assert r.returncode==(0 if ok else 2),(r.returncode,r.stdout,r.stderr)
            if ok:
                assert Path(r.stdout)==bundled
                assert subprocess.check_output([r.stdout],cwd=cwd,env=env,text=True).strip()=='shared compiler invoked'
            else:
                assert not r.stdout
                assert offender.replace('\\','/') in r.stderr.replace('\\','/') and 'repair or reinstall the shared compiler store, or rebuild as portable' in r.stderr,r.stderr
        run(True)
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
        kernel.CreateFileW.restype=ctypes.c_void_p
        kernel.CloseHandle.argtypes=[ctypes.c_void_p]
        lock=str(store)+'.lock'
        def exclusive():
            return kernel.CreateFileW(lock,0xC0000000,0,None,4,0x80,None)
        invalid=ctypes.c_void_p(-1).value
        holder=subprocess.Popen([str(probe),str(toolkit),'hold'],cwd=cwd,env=env,text=True,stdin=subprocess.PIPE,stdout=subprocess.PIPE)
        try:
            assert Path(holder.stdout.readline().strip())==bundled
            assert exclusive()==invalid and ctypes.get_last_error()==32
            run(True) # A second shared runtime can use the same immutable store.
        finally:
            holder.communicate('\n',timeout=10)
        handle=exclusive()
        assert handle!=invalid
        try: run(False,lock)
        finally: kernel.CloseHandle(handle)
        saved=(store/'lib/support.dll').read_bytes()
        (store/'lib/support.dll').write_bytes(b'corrupt')
        run(False,'support.dll')
        (store/'lib/support.dll').write_bytes(saved)
        (store/'SHA256SUMS.txt').write_bytes(manifest.encode()+b'corrupt')
        run(False,'SHA256SUMS.txt')
        (store/'SHA256SUMS.txt').write_bytes(manifest.encode())
        for member in ('lib/support.dll','bin/clang.exe','SHA256SUMS.txt'):
            path=store/member
            parked=path.with_name(path.name+'.parked')
            path.rename(parked)
            try: run(False, str(path))
            finally: parked.rename(path)
        parked=store.with_name(store.name+'.parked')
        store.rename(parked)
        try: run(False,str(store))
        finally: parked.rename(store)
        marker.write_text('invalid\n')
        run(False,'compiler-store.sha256')
        marker.write_bytes((content+'\n').encode())
        (toolkit/'compiler.txt').unlink()
        run(False,'compiler.txt')
        print('PASS: enclosing void startup, valid shared invocation, corrupt support, missing member/compiler/manifest/store, bad marker/config, host decoy rejected')


if __name__=='__main__':
    main()
