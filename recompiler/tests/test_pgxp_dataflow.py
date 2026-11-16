#!/usr/bin/env python3
"""Compile actual emitted GTE-to-packet code against the full PGXP engine."""
import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'runtime/tests'))
from fixture_link import nm_symbols, library_symbols, write_stubs, MULTIPLE, UNDEFINED


def run(argv, verbose=True, **kw):
    p = subprocess.run([str(x) for x in argv], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', **kw)
    if p.returncode:
        raise RuntimeError(' '.join(map(str, argv)) + '\nexit code: %d (0x%08X)\n' % (p.returncode, p.returncode & 0xffffffff) + p.stdout + p.stderr)
    if p.stdout and verbose:
        print(p.stdout, end='', flush=True)
    return p


def program():
    base = 0x80010000
    words, labels, branches = [], {}, []
    def i(op, rs, rt, imm=0): words.append(op << 26 | rs << 21 | rt << 16 | (imm & 65535))
    def r(rs, rt, rd, fn, sh=0): words.append(rs << 21 | rt << 16 | rd << 11 | sh << 6 | fn)
    words.append(0x48087000)  # mfc2 t0,SXY2
    i(15, 0, 10, 0x1234)  # shadow-dead control setup, even across the loop
    i(9, 0, 9, 0)
    labels['loop'] = len(words)
    i(9, 9, 9, 1)
    i(10, 9, 10, 4)
    branches.append((len(words), 'loop')); i(5, 10, 0)
    words.append(0)
    r(0, 8, 11, 0, 16); r(0, 11, 11, 3, 16); i(9, 11, 11, 1)
    r(0, 8, 12, 2, 16); i(9, 12, 12, -1); r(0, 12, 12, 0, 16)
    i(12, 11, 11, 65535); r(11, 12, 13, 0x25); i(43, 4, 13)
    i(35, 5, 8); words.append(0); r(8, 6, 8, 0x21); i(43, 4, 8, 4)
    words.append(0x480E7000)  # mfc2 t6,SXY2
    r(14, 0, 10, 0x21)  # dead coordinate copy, destructive queue must end lineage
    i(15, 0, 10, 12); i(13, 10, 10, 8); i(43, 4, 10, 8)
    i(35, 7, 15); words.append(0); i(43, 4, 15, 12)
    r(8, 0, 16, 0x21); i(0x22, 5, 16, 1); i(0x26, 5, 16, 2); i(43, 4, 16, 16)
    i(0x23, 5, 18); i(0x22, 5, 18, 1); i(43, 4, 18, 20)  # load-delay merge forwarding
    words += [0x03E00008, 0]
    for index, label in branches:
        words[index] |= (labels[label] - index - 1) & 65535
    # A conditional tail-transfer is deliberately absent from the first
    # function's local CFG successor list. Its old s3 must remain caller-live.
    words += [0] * (64-len(words))
    i(9, 0, 19, 0x1357); i(4, 6, 0, 62); words.append(0)
    i(9, 0, 19, 0x2468); words += [0x03E00008, 0]
    words += [0] * (128-len(words))
    i(43, 4, 19, 24); words += [0x03E00008, 0]
    # Whole nongeometry load/ALU operation with boundary-owned receipts.
    words += [0] * (192-len(words))
    i(15, 0, 12, 0x8000); i(35, 12, 2, 0x2000); words.append(0)
    i(9, 2, 2, 7); i(10, 2, 3, 0x7fff); words += [0x03E00008, 0]
    words += [0] * (256-len(words))
    i(9, 20, 2, 1); words += [0x03E00008, 0]
    # One mixed operation, with all three store widths and real write/read
    # aliasing. The native span already charged before admission; static
    # psx_store_cycle_barrier stays present and does not publish a new clock.
    words += [0] * (320-len(words))
    i(15, 0, 12, 0x8000); i(35, 12, 2, 0x2000); words.append(0)
    i(9, 2, 2, 7); i(15, 0, 13, 0x8002); i(43, 13, 2)
    i(9, 2, 2, 1); i(41, 13, 2, 4); i(40, 13, 2, 6)
    i(35, 13, 3); words.append(0); words += [0x03E00008, 0]
    # A cold partial destination followed by LW needs canonical validation
    # of untouched bits, even though its GPR source is entirely integer.
    words += [0] * (384-len(words))
    i(15, 0, 13, 0x8002); i(41, 13, 20, 8); i(35, 13, 2, 8)
    words.append(0); words += [0x03E00008, 0]
    return base, words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--recompiler', required=True)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--cxx', required=True)
    ap.add_argument('--work-dir', type=Path,
                    help='Retain generated sources, objects and executable for a focused diagnosis')
    ap.add_argument('--operations', action='store_true', help='qualify guarded whole integer operations and write ownership')
    a = ap.parse_args()
    with (nullcontext(str(a.work_dir)) if a.work_dir else
          tempfile.TemporaryDirectory(prefix='psx-pgxp-flow-')) as folder:
        work = Path(folder)
        if a.work_dir:
            work.mkdir(parents=True, exist_ok=False)
        base, words = program()
        header = bytearray(2048); header[:8] = b'PS-X EXE'
        for offset, value in ((0x10, base), (0x18, base), (0x1C, 4*len(words))):
            struct.pack_into('<I', header, offset, value)
        exe = work / 'flow.psx'; exe.write_bytes(header + struct.pack('<%dI' % len(words), *words))
        seeds = work / 'seeds.txt'; seeds.write_text('0x80010000\n0x80010100\n0x80010200\n0x80010300\n0x80010400\n0x80010500\n0x80010600\n', encoding='utf-8')
        out = work / 'generated'
        run([a.recompiler, exe, '--seeds', seeds, '--out-dir', out, '--project-root', ROOT], verbose=False)
        sources = sorted(out.glob('*_full*.c'))
        emitted = [x.read_text(encoding='utf-8') for x in sources]
        if not emitted or any('psx-pgxp-dataflow-v2' not in x[:2048] for x in emitted):
            raise RuntimeError('actual CLI generation lacks the precision-lifetimes v2 marker')
        text = '\n'.join(emitted)
        dead_words = {int(x, 16) for x in re.findall(r'PGXP_FLOW_(?:DEAD|ELIDED)_ALU\(\d+u, 0x([0-9A-F]+)u', text)}
        elided_words = {int(x, 16) for x in re.findall(r'PGXP_FLOW_ELIDED_ALU\(\d+u, 0x([0-9A-F]+)u', text)}
        if words[21] not in elided_words or words[1] in elided_words or words[4] in elided_words:
            raise RuntimeError('actual lifetime elision crossed an observation or kept an overwritten record')
        if not {words[1], words[4], words[21]} <= dead_words:
            raise RuntimeError('actual CFG emitter failed to remove cross-block control/dead-copy hooks')
        for word in words[7:15] + [words[18], words[28]]:
            if word in dead_words:
                raise RuntimeError('actual emitter removed live coordinate arithmetic 0x%08X' % word)
        if words[64] in dead_words:
            raise RuntimeError('actual emitter lost a conditional external caller boundary')
        print('Actual emitted precision hooks: %d elided lifetimes, %d deferred dead writes, %d retained ALU hooks' %
              (len(re.findall(r'PGXP_FLOW_ELIDED_ALU\(', text)),
               len(re.findall(r'PGXP_FLOW_DEAD_ALU\(', text)),
               len(re.findall(r'PGXP_(?:PLAIN_)?ALU\(', text))), flush=True)
        if a.operations:
            for address in ('80010300','80010500'):
                # An overlapping function can be an entry wrapper AFTER its
                # shared alias body. Inspect the actual executing block, never
                # the next unrelated operation following that public wrapper.
                entries = list(re.finditer(r'(?m)^block_'+address+r':\s*\n', text))
                if len(entries)!=1:
                    raise RuntimeError('actual nongeometry block entry is not unique: '+address)
                entry = entries[0].end()
                boundary = re.search(r'(?m)^(?:block_[0-9A-Fa-f]{8}:|(?:static )?void (?:func_|psx_alias_body_))', text[entry:])
                stop = entry+boundary.start() if boundary else len(text)
                start = text.index('psx-pgxp-integer-operation-v2', entry, stop)
                end = text.index('} else\n#endif', start, stop)
                if re.search(r'\bPGXP_(?:ALU|LOAD|STORE|COP2|MULDIV|FLOW_|PLAIN_)', text[start:end]):
                    raise RuntimeError('actual nongeometry operation retained an instruction hook')
                if 'psx_pgxp_operation_begin' not in text[start:end] or 'psx_pgxp_operation_end' not in text[start:end]:
                    raise RuntimeError('actual nongeometry operation lacks its boundary proof/receipt')
                if address=='80010500' and ('PSX_STORE_WORD' not in text[start:end] or
                    'PSX_STORE_HALF' not in text[start:end] or 'PSX_STORE_BYTE' not in text[start:end] or
                    text[start:end].count('psx_store_cycle_barrier()')!=3):
                    raise RuntimeError('actual mixed operation lost a maintained store/barrier')
            print('Actual nongeometry load/ALU/store operations: zero instruction PGXP hooks; all widths/barriers and canonical fallback retained', flush=True)
        common = ['-O2', '-ffunction-sections', '-fdata-sections', '-DPSX_PGXP=1',
                  '-DPSX_NATIVE_CPU_SPANS=1', '-DPSX_DIRECT_GUEST_MEMORY=1', '-DPSX_NO_DEBUG_TOOLS=1', '-Werror',
                  '-I', str(ROOT/'runtime/include'), '-I', str(out)]
        objects = []
        for selected, name in ((0, 'reference'), (1, 'candidate')):
            c = work / (name+'.c')
            names = set(re.findall(r'\b(?:func_|psx_alias_body_)[0-9A-Fa-f]{8}\b', text))
            aliases = ''.join('#define %s %s\n' % (symbol, 'flow_'+name if symbol=='func_80010000'
                              else 'flow_'+name+'_'+symbol) for symbol in sorted(names))
            c.write_text(aliases+text, encoding='utf-8')
            obj = work / (name+'.o')
            run([a.cc, *common, '-DPSX_PGXP_DATAFLOW='+str(selected), '-DPSX_PGXP_OPERATIONS='+str(int(a.operations and selected)), '-c', c, '-o', obj])
            objects.append(obj)
        for compiler, source, name in (
            (a.cc, ROOT/'runtime/src/psx_ram_geometry.c', 'ram'),
            (a.cxx, ROOT/'runtime/src/pgxp.cpp', 'pgxp'),
            (a.cxx, ROOT/'runtime/tests/pgxp_test_mod_memory.cpp', 'mod-memory'),
            (a.cxx, ROOT/'runtime/tests/pgxp_dataflow_fixture.cpp', 'fixture')):
            obj = work/(name+'.o')
            run([compiler, *common, '-DPSX_PGXP_DATAFLOW=1', '-DPSX_PGXP_OPERATIONS='+str(int(a.operations)), '-c', source, '-o', obj]); objects.append(obj)
        defined, undefined = nm_symbols(objects)
        provided = library_symbols(a.cc)
        stubs = {s for s in undefined-defined-provided if not s.startswith('__')}
        if any('pgxp_operation' in s or 'pgxp_write_owned' in s for s in stubs):
            raise RuntimeError('candidate precision implementation is missing; cannot substitute a linker seam')
        result_exe = work/('products.exe' if os.name == 'nt' else 'products')
        for _ in range(20):
            write_stubs(work/'stubs.c', stubs)
            run([a.cc, '-w', '-c', work/'stubs.c', '-o', work/'stubs.o'])
            p = subprocess.run([a.cxx, *map(str, objects), str(work/'stubs.o'),
                                '-Wl,--gc-sections', '-lm', '-o', str(result_exe)], capture_output=True, text=True)
            if not p.returncode: break
            changed = False
            for symbol in MULTIPLE.findall(p.stderr):
                if symbol in stubs: stubs.remove(symbol); changed = True
            for symbol in UNDEFINED.findall(p.stderr):
                if symbol not in defined and symbol not in provided and symbol not in stubs:
                    stubs.add(symbol); changed = True
            if not changed: raise RuntimeError(p.stderr)
        else: raise RuntimeError('fixture link did not converge')
        run([result_exe])
    return 0


if __name__ == '__main__':
    sys.exit(main())
