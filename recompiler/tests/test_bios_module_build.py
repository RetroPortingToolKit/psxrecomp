#!/usr/bin/env python3
"""tools/bios_module_build.py end to end against the real emitter.

Builds a loadable backend from the bundled, redistributable OpenBIOS image
(the only image this tree can legally test with), then loads the result in
this interpreter and reads the descriptor back through ctypes:

  * the module is self-contained (RTLD_NOW resolves everything or fails;
    on Windows the -shared link itself refuses an unresolved symbol, so
    there the test builds the module and skips only the ctypes read),
  * it exports exactly the psx_bios_module.h entry points,
  * its ABI tag carries the requested flavor and its codegen hash is the one
    runtime/include/overlay_codegen_hash.h (or overlay_api.h's default) names,
  * the descriptor's image identity is OpenBIOS's real size and CRC-32,
  * a dump that does not match the profile's SHA-256 pin is refused before
    anything is compiled.

Needs the built psxrecomp-bios (--emitter, default recompiler/build) and a C
compiler on PATH; skips (exit 0 with a notice) when either is absent, since a
BIOS-free tree cannot prove anything here.

    python3 recompiler/tests/test_bios_module_build.py [--emitter PATH]
"""

import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TOOLS = os.path.join(ROOT, 'tools')


def abi_version_expected():
    p = os.path.join(ROOT, 'runtime', 'include', 'psx_bios_module.h')
    m = re.search(r'#define\s+PSX_BIOS_MODULE_ABI_VERSION\s+(\d+)', open(p).read())
    assert m, f'PSX_BIOS_MODULE_ABI_VERSION not found in {p}'
    return int(m.group(1))


def codegen_hash_expected():
    p = os.path.join(ROOT, 'runtime', 'include', 'overlay_codegen_hash.h')
    if os.path.isfile(p):
        m = re.search(r'PSX_OVERLAY_CODEGEN_HASH\s+(0x[0-9a-fA-F]+)', open(p).read())
        if m:
            return int(m.group(1), 16)
    return None   # header absent: overlay_api.h defaults to 0u


class PsxBiosImageInfo(ctypes.Structure):
    _fields_ = [('kbless_ram_lo', ctypes.c_uint32), ('kbless_ram_hi', ctypes.c_uint32),
                ('kbless_rom_off', ctypes.c_uint32), ('shell_entry_phys', ctypes.c_uint32),
                ('deliver_event_ret', ctypes.c_uint32), ('image_size', ctypes.c_uint32),
                ('image_crc32', ctypes.c_uint32), ('image_wordsum', ctypes.c_uint32),
                ('image_sha256', ctypes.c_char_p), ('image_id', ctypes.c_char_p),
                ('image_bundled', ctypes.c_int)]


class PsxBiosBackend(ctypes.Structure):
    _fields_ = [('image', ctypes.POINTER(PsxBiosImageInfo)),
                ('dispatch', ctypes.c_void_p), ('dispatch_call', ctypes.c_void_p),
                ('kernel_bodies', ctypes.c_void_p), ('kernel_body_count', ctypes.c_uint32),
                ('kernel_patch_ranges', ctypes.c_void_p), ('kernel_patch_range_count', ctypes.c_uint32)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emitter', default=None)
    args = ap.parse_args()
    emitter = args.emitter
    if not emitter:
        for c in ('recompiler/build/psxrecomp-bios', 'recompiler/build/psxrecomp-bios.exe',
                  'recompiler/build-t2/psxrecomp-bios'):
            if os.path.isfile(os.path.join(ROOT, c)):
                emitter = os.path.join(ROOT, c)
                break
    if not emitter or not os.path.isfile(emitter):
        print('SKIP: psxrecomp-bios not built (pass --emitter)')
        return 0
    cc = shutil.which('gcc') or shutil.which('cc') or shutil.which('clang')
    if not cc:
        print('SKIP: no C compiler on PATH')
        return 0
    if codegen_hash_expected() is None:
        print('SKIP: runtime/include/overlay_codegen_hash.h is not generated yet (a runtime '
              'build writes it); the builder refuses a module without it')
        return 0
    windows = sys.platform.startswith('win')

    dump = os.path.join(ROOT, 'bios', 'openbios.bin')
    with open(dump, 'rb') as f:
        data = f.read()
    want_crc = zlib.crc32(data) & 0xFFFFFFFF

    work = tempfile.mkdtemp(prefix='bios_module_test_')
    try:
        out = os.path.join(work, 'OpenBIOS_test.dll' if windows else 'OpenBIOS_test.so')
        base = [sys.executable, os.path.join(TOOLS, 'bios_module_build.py'),
                '--toolchain', work,            # no staged layout: every piece overridden
                '--emitter', emitter,
                '--include', os.path.join(ROOT, 'runtime', 'include'),
                '--profiles', os.path.join(ROOT, 'bios'),
                '--seeds', os.path.join(ROOT, 'recompiler', 'seeds'),
                '--stem', 'OpenBIOS', '--compiler', 'gcc', '--gcc', cc]

        # A wrong image is refused by the profile's SHA-256 pin, before compiling.
        wrong = os.path.join(work, 'wrong.bin')
        with open(wrong, 'wb') as f:
            f.write(bytes(len(data)))
        r = subprocess.run(base + ['--dump', wrong, '--out', out, '--flavor', '0'],
                           capture_output=True, text=True)
        assert r.returncode != 0, 'a zero-filled image must be refused'
        assert 'not OPENBIOS' in r.stdout or 'is not' in r.stdout, r.stdout[-600:]
        assert not os.path.exists(out), 'refusal must leave no module'

        # The real image builds, with flavor 2 so the tag's flavor bits are exercised.
        r = subprocess.run(base + ['--dump', dump, '--out', out, '--flavor', '2'],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-1000:]
        assert f'PSX_BIOS_MODULE_PUBLISHED {out}' in r.stdout, r.stdout[-600:]
        assert os.path.isfile(out)
        if windows:
            print('OK: OpenBIOS module built and linked; SKIP: the ctypes descriptor read '
                  'is validated on POSIX, the host test covers Windows loading')
            return 0

        lib = ctypes.CDLL(out)   # RTLD_NOW: an unresolved symbol fails here
        lib.psx_bios_module_abi.restype = ctypes.c_int
        lib.psx_bios_module_codegen_hash.restype = ctypes.c_uint32
        lib.psx_bios_module_backend.restype = ctypes.POINTER(PsxBiosBackend)
        abi = lib.psx_bios_module_abi()
        assert (abi & 0xFFFF) == abi_version_expected(), f'ABI version {abi & 0xFFFF}'
        assert ((abi >> 16) & 0xFFFF) == 2, f'flavor bits {(abi >> 16) & 0xFFFF}'
        h = lib.psx_bios_module_codegen_hash()
        want_h = codegen_hash_expected()
        if want_h is not None:
            assert h == want_h, f'codegen hash {h:#x} != header {want_h:#x}'
        b = lib.psx_bios_module_backend().contents
        img = b.image.contents
        assert img.image_id == b'OPENBIOS', img.image_id
        assert img.image_size == len(data), img.image_size
        assert img.image_crc32 == want_crc, f'{img.image_crc32:#x} != {want_crc:#x}'
        assert img.image_bundled == 1
        assert b.dispatch and b.dispatch_call, 'dispatch entry points missing'
        assert b.kernel_body_count > 0, 'no kernel bodies published'

        # The CPS constructor must not be in a module (it would write a host global).
        nm = shutil.which('nm')
        if nm:
            syms = subprocess.run([nm, '-D', '--undefined-only', out], capture_output=True, text=True).stdout
            assert 'g_psx_cps_mode' not in syms, 'module still references g_psx_cps_mode'
            names = [ln.split()[-1] for ln in syms.splitlines() if ln.strip()]
            bad = [n for n in names if not n.startswith('_') and '@' not in n]
            assert not bad, f'module imports non-libc symbols: {bad}'
        print(f'OK: OpenBIOS module built and loaded (abi={abi:#x}, hash={h:#x}, crc={img.image_crc32:08X})')
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
