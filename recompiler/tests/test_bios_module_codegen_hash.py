#!/usr/bin/env python3
"""A BIOS module always exports the codegen hash of the includes it built from.

The bug this pins (issue #7, "freshly built module rejected: codegen hash
00000000 != host 1b6950cb"): overlay_api.h defines PSX_OVERLAY_CODEGEN_HASH
only inside `#if defined(__has_include)`, and tcc 0.9.27 does not implement
`__has_include` at all. So on the tcc tier the fallback `0u` won and every
module exported hash 0. psx_bios_module_load() then rejected the module it had
just built. gcc and clang implement the macro, so the development path never
saw it: the bug reached only players with no compiler on PATH, which is
exactly the case the bundled tcc exists for.

Two independent things are asserted, because each was a real gap:

  * the hash reaches the compiler as an explicit -D, so no compiler's
    preprocessor decides the identity of the module; and
  * the self-check COMPARES the exported hash instead of printing it, so a
    mismatch is reported by the builder rather than by the host afterwards.

Needs no compiler, no emitter and no built target.

    python3 recompiler/tests/test_bios_module_codegen_hash.py
"""

import ast
import os
import re
import shutil
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TOOLS = os.path.join(ROOT, 'tools')
BUILDER = os.path.join(TOOLS, 'bios_module_build.py')
MIRROR = os.path.join(ROOT, 'overlay_toolchain', 'bios_module_build.py')

HASH = 0x1B6950CB


def load_compile_module():
    """compile_module from bios_module_build.py, without importing the tool.

    The module imports compile_overlays, which needs the rest of the toolchain;
    the function under test only needs its source. compile_overlays is stubbed
    for the tcc/gcc helpers it reaches for, so the emitted command line can be
    inspected without a compiler on the machine."""
    with open(BUILDER, encoding='utf-8') as f:
        text = f.read()
    tree = ast.parse(text, filename=BUILDER)
    node = next((n for n in tree.body
                 if isinstance(n, ast.FunctionDef) and n.name == 'compile_module'), None)
    assert node is not None, 'bios_module_build.py no longer defines compile_module'
    # MODULE_DEFINES is the base every module compile is built from; the define
    # under test is appended to it, so it has to come along.
    base = [n for n in tree.body if isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id == 'MODULE_DEFINES' for t in n.targets)]
    assert base, 'bios_module_build.py no longer defines MODULE_DEFINES'

    captured = []

    class Stub:
        """Absorbs every compile_overlays helper compile_module touches."""
        _TCC_COMPAT_PREFIX = b''
        _TCC_BOMFREE_INC = {}

        def __getattr__(self, name):
            return lambda *a, **k: None

        @staticmethod
        def native_path(p):
            return p

        @staticmethod
        def _bom_free_incdir(d):
            return d

        @staticmethod
        def _toolchain_env(gcc):
            return None, None

        @staticmethod
        def target_arch_flags(_c):
            return []

        @staticmethod
        def is_windows():
            return True

    def run(cmd, **_kw):
        captured.append(cmd)

        class R:
            returncode = 0
            stdout = ''
            stderr = ''
        return R()

    scope = {'os': os, 'subprocess': type('S', (), {'run': staticmethod(run)}),
             'co': Stub()}
    exec(compile(ast.Module(body=base + [node], type_ignores=[]), BUILDER, 'exec'), scope)
    return scope['compile_module'], captured


def main():
    compile_module, captured = load_compile_module()
    tmp = tempfile.mkdtemp(prefix='bios_hash_')
    made = []
    try:
        src = os.path.join(tmp, 'stub.c')
        with open(src, 'w') as f:
            f.write('/* stub */\n')
        out = os.path.join(tmp, 'out.dll')

        # ---- the hash reaches BOTH compiler tiers as an explicit -D ---------
        for compiler in ('tcc', 'gcc'):
            captured.clear()
            compile_module([src], out, [tmp], 'SCPH1001', 0, HASH,
                           compiler, 'gcc', 'tcc')
            assert captured, f'{compiler}: no compiler invocation was made'
            cmd = captured[-1]
            flag = '-DPSX_OVERLAY_CODEGEN_HASH=0x%08xu' % HASH
            assert flag in cmd, (
                f'{compiler} was not told the codegen hash; it would fall back to '
                f'the __has_include probe in overlay_api.h. Command: {cmd}')
            # The stale form: a module built without this define exports 0u on
            # any compiler without __has_include, which is the reported failure.
            assert '-DPSX_OVERLAY_CODEGEN_HASH=0x00000000u' not in cmd

        # ---- the self-check COMPARES the hash, it does not just print it ----
        with open(BUILDER, encoding='utf-8') as f:
            text = f.read()
        self_check = next((n for n in ast.parse(text).body
                           if isinstance(n, ast.FunctionDef) and n.name == 'self_check'), None)
        assert self_check is not None, 'bios_module_build.py no longer defines self_check'
        params = [a.arg for a in self_check.args.args]
        assert 'codegen_hash' in params, (
            'self_check cannot assert the exported hash: it takes %r' % params)
        body = ast.dump(self_check)
        assert 'Compare' in body, (
            'self_check loads the module but never compares the codegen hash, '
            'so a wrong hash still reports success')

        # ---- a tree that cannot name the hash must fail, not publish --------
        assert re.search(r'if\s+codegen_hash\s*==\s*0\s*:', text), (
            'a missing/zero overlay_codegen_hash.h must be a hard error: the '
            'module would export a hash the host rejects')

        # ---- the shipped mirror must not drift from the canonical builder ---
        if os.path.isfile(MIRROR):
            with open(MIRROR, encoding='utf-8') as f:
                mirror = f.read()
            m = re.search(r'-DPSX_OVERLAY_CODEGEN_HASH=0x%08xu', mirror)
            assert m, (
                'overlay_toolchain/bios_module_build.py predates the explicit '
                'codegen-hash define; the mirror ships inside releases')
        else:
            print('note: overlay_toolchain/ mirror not present in this tree')

        print('OK: codegen hash is passed explicitly to tcc and gcc, asserted by '
              'the self-check, and a missing header is fatal')
        return 0
    finally:
        for d in made:
            shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
