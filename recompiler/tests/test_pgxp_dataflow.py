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


def add_region_program(base,words):
    words+= [0]*(448-len(words))
    # Both paths define s1 from incoming v1. One path then overwrites v1
    # with an equal-bit LUI before the join: a deferred s1 must retain the OLD
    # source precision, not read the newly materialized v1 at that join.
    main=[0x27bdffe8,0xafbf0010,0x3c088000,0x25087000,0x2a09000a,
          0x11200003,0x24710001,0x08000000|(((base+448*4+40)>>2)&0x3ffffff),0x3c038000,
          0x35080080,0x0c004200,0, # JAL80010800
          0x3c0a8000,0x354a8000,0x254affff,0xac8a0000,0x8fbf0010,0,
          0x03e00008,0x27bd0018]
    words+=main
    words += [0]*(512-len(words));words += [0x3c028000,0x34421234,0x03e00008,0]
    # Contiguous after the child JR/delay: NOP padding previously let discovery
    # create an overlapping alias host, which cannot qualify a private entry.
    words += [0x2694ffff,0x3c088000,0x25087000,0x1e80fffc,0,0x03e00008,0]
    # Same balanced SP/RA scaffold, with real MFC2/arithmetic/memory packet
    # outputs. Its explicit contract leaves the existing opaque-observer cases
    # above canonical; this is not a relaxation of their full-receipt checks.
    words += [0x27bdffe8,0xafbf0010,0x48087000,0x01004821,0x25290001,
              0xac890000,0x8caa0000,0,0xac8a0004,0x8fbf0010,0,
              0x03e00008,0x27bd0018]
    return base,words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--recompiler', required=True)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--cxx', required=True)
    ap.add_argument('--work-dir', type=Path,
                    help='Retain generated sources, objects and executable for a focused diagnosis')
    ap.add_argument('--compile-only', action='store_true', help='prepare the exact unit executable without rerunning qualified products')
    ap.add_argument('--reuse-passed-source', type=Path, help='O0 only: verify unchanged passed bodies and run remaining region cases')
    ap.add_argument('--geometry-control', action='store_true', help='existing regions products plus certified geometry-only control-frame outputs')
    ap.add_argument('--regions', action='store_true', help='actual private CFG/owner/clock/opaque-entry products')
    ap.add_argument('--tail-compile', action='store_true', help='O0 regions only: additionally compile the same candidate with the supported native-tail profile')
    ap.add_argument('--runtime-root', type=Path, default=ROOT, help='verified production headers/project root for test-only fixture worktree')
    ap.add_argument('--dispatch', type=Path, help='already verified actual normal BIOS dispatch for owner extraction')
    ap.add_argument('--opt', choices=('0','2'), default='2')
    ap.add_argument('--operations', action='store_true', help='qualify guarded whole integer operations and write ownership')
    a = ap.parse_args()
    if a.geometry_control and not a.regions:ap.error('--geometry-control requires --regions')
    if a.tail_compile and (not a.regions or a.opt!='0'):
        ap.error('--tail-compile is the single supported-profile O0 regions compile check')
    if a.reuse_passed_source and (not a.regions or a.opt!='0'):
        ap.error('--reuse-passed-source is only for remaining O0 region products')
    with (nullcontext(str(a.work_dir)) if a.work_dir else
          tempfile.TemporaryDirectory(prefix='psx-pgxp-flow-')) as folder:
        work = Path(folder)
        if a.work_dir:
            work.mkdir(parents=True, exist_ok=False)
        base, words = program()
        if a.regions:
            if a.operations or not a.dispatch: ap.error("--regions requires --dispatch and OPERATIONS remains OFF")
            base,words=add_region_program(base,words)
        header = bytearray(2048); header[:8] = b'PS-X EXE'
        for offset, value in ((0x10, base), (0x18, base), (0x1C, 4*len(words))):
            struct.pack_into('<I', header, offset, value)
        exe = work / 'flow.psx'; exe.write_bytes(header + struct.pack('<%dI' % len(words), *words))
        seeds = work / 'seeds.txt'; seeds.write_text('0x80010000\n0x80010100\n0x80010200\n0x80010300\n0x80010400\n0x80010500\n0x80010600\n'+('0x80010700\n0x80010800\n0x80010810\n0x8001082C\n' if a.regions else ''), encoding='utf-8')
        out = work / 'generated'
        generate=[a.recompiler, exe, '--seeds', seeds, '--out-dir', out, '--project-root', a.runtime_root]
        if a.regions:
            config=work/'game.toml'
            config.write_text('[game]\nname="Precision Regions"\nid="TEST-00000"\nexe="flow.psx"\n'
                'entry_pc="0x80010000"\nload_address="0x80010000"\nstack_base="0x801FFFF0"\n'
                +'text_size="0x%X"\n'%(len(words)*4)+
                '[recompiler]\nseeds="seeds.txt"\nout_dir="generated"\nmod_function_entry_funcs=["0x80010700","0x80010800","0x80010810"]\n'
                'mod_instruction_sites=["0x80010804"]\n'+
                ('geometry_control_frames={"0x8001082C"="fixture_geometry_control_allowed"}\n' if a.geometry_control else ''),encoding='utf-8')
            generate+=['--ws-config',config]
        run(generate, verbose=False)
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
        if a.regions:
            from test_static_dispatch_owner import function
            dispatch=a.dispatch.read_text(encoding='utf-8')
            owner=(function(dispatch,'psx_dispatch_chain_hooks') if 'PSX_NATIVE_TAIL_OWNER_V1' in dispatch else '')+function(dispatch,'psx_dispatch_check_return_boundary')+function(dispatch,'psx_dispatch_impl')
            owner,changed=re.subn(r'int lo = 0, hi = \d+ - 1;','int lo = 0, hi = 1 - 1;',owner)
            assert changed==1
            (work/'owner.inc').write_text(owner,encoding='utf-8')
            (work/'psx_observation_config.h').write_text('#pragma once\n#define PSX_OBSERVATION_WAIT_SITE(pc) 0\n',encoding='utf-8')
            def body(address,source=text):
                start=source.index('void func_'+address+'(CPUState* cpu)\n{')
                return function('static '+source[start:],'func_'+address)
            if a.reuse_passed_source:
                passed=a.reuse_passed_source.read_text(encoding='utf-8')
                for address in ('80010000','80010700','80010800'):
                    if body(address)!=body(address,passed):
                        raise RuntimeError('passed executing body changed; cannot reuse: '+address)
                print('REUSE actual passed 38 GTE products + region modes0/1: exact executing body source equality verified',flush=True)
            main_body=body('80010700')
            private=main_body.split('/* precision-demand-regions:',1)[1]
            for word in ('27BDFFE8','27BD0018'):
                assert 'PGXP_ALU(0x'+word+'u' not in private
                assert re.search(r'0x'+word+r'[^\n]*precision-demand: native integer definition',private)
                assert 'PGXP_ALU(0x'+word+'u' in main_body.split('/* precision-demand-regions:',1)[0]
            assert 'psx_pgxp_region_add_immediates' in text
            if a.geometry_control:
                control=body('8001082C').split('/* precision-demand-regions:',1)[1]
                for hook in ('PGXP_ALU(0x27BDFFE8','PGXP_ALU(0x27BD0018',
                             'PGXP_STORE(0xAFBF0010','PGXP_LOAD(0x8FBF0010'):
                    assert hook not in control
                assert control.count('precision-demand: geometry-dead control frame')==4
                assert 'PGXP_COP2' in control and 'PGXP_STORE' in control and 'PGXP_LOAD' in control

            charge=re.search(r'psx_native_span_charge\((\d+)u\)',main_body)
            assert charge and 'precision_block_80010700:' in main_body and 'psx_block_edge_materialize' in main_body
            assert 'psx_precision_block_edge_materialize' not in text
            loop_body=body('80010810')
            assert 'precision_block_80010810:' in loop_body
            assert 'psx_mod_function_entry(cpu, 0x80010810u)' in loop_body
            assert 'psx_alias_body_' not in loop_body[:loop_body.index('precision_block_80010810:')]
            (work/'region_case.h').write_text('#define REGION_FIRST_CHARGE '+charge[1]+'u\n'
                '#define REGION_CONT_PC 0x80010730u\n#define REGION_INTERIOR_PC 0x80010724u\n#define REGION_LOOP_PC 0x80010810u\n'
                '#define REGION_CODE_BASE 0x80010000u\n#define REGION_CODE_BYTES '+str(len(words)*4)+'u\n'
                'static const unsigned char region_code[]={'+','.join(str(x) for x in struct.pack('<%dI'%len(words),*words))+'};\n',encoding='utf-8')
            for line in main_body.splitlines():
                if 'precision-demand: native integer definition' in line:
                    assert 'PGXP_' not in line
            print('Actual emitted private/canonical CFG owner entry, CPS and materializing-edge source guards PASS',flush=True)
        common = ['-O'+a.opt, '-ffunction-sections', '-fdata-sections', '-DPSX_PGXP=1',
                  '-DPSX_NATIVE_CPU_SPANS=1', '-DPSX_DIRECT_GUEST_MEMORY=1', '-DPSX_NO_DEBUG_TOOLS=1', '-Werror',
                  '-I', str(a.runtime_root/'runtime/include'), '-I', str(out)]
        if a.regions:
            common += ['-DPSX_TEST_PGXP_REGIONS=1','-DPSX_ENABLE_BLOCK_CYCLES=1','-DPSX_OBSERVATION_TIME=1',
                       '-DPSX_NATIVE_STATIC_CALLS=1','-DPSX_NATIVE_TAIL_CHAINS=0','-DPSX_HAS_GAME_DISPATCH',
                       '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-I',str(work)]
        if a.geometry_control:common+=['-DPSX_TEST_GEOMETRY_CONTROL=1']
        if a.reuse_passed_source:common+=['-DPSX_TEST_PGXP_REGIONS_REMAINING=1']
        objects = []
        for selected, name in ((0, 'reference'), (1, 'candidate')):
            c = work / (name+'.c')
            names = set(re.findall(r'\b(?:func_|psx_alias_body_)[0-9A-Fa-f]{8}\b', text))
            aliases = ''.join('#define %s %s\n' % (symbol, 'flow_'+name if symbol=='func_80010000'
                              else 'flow_'+name+'_'+symbol) for symbol in sorted(names))
            c.write_text(aliases+text, encoding='utf-8')
            obj = work / (name+'.o')
            selection = ['-DPSX_PGXP_DATAFLOW='+str(0 if a.regions else selected),
                         '-DPSX_PGXP_DEMAND_REGIONS='+str(int(a.regions and selected)),
                         '-DPSX_PGXP_OPERATIONS='+str(int(a.operations and selected))]
            if a.regions:
                # Inspect the actual selected C, not mutually-exclusive source
                # text: the native compiler must see only its private CFG.
                pp = run([a.cc, *common, *selection, '-E', '-P', c], verbose=False).stdout
                owner_name = 'flow_'+name+'_func_80010700'
                definition = re.search(r'\bvoid '+owner_name+r'\(\s*CPUState\s*\*\s*cpu\s*\)\s*\{', pp)
                assert definition
                compiled = function('static '+pp[definition.start():], owner_name)
                if selected:
                    cold = function(pp, 'func_80010700_precision_canonical')
                    assert 'precision_block_80010700:' in compiled
                    assert not re.search(r'(?m)^block_[0-9A-F]{8}:', compiled)
                    assert 'block_80010700:' in cold and 'precision_block_' not in cold
                    assert 'case 0x80010700u:' in cold and 'case 0x80010730u:' in cold
                    assert '_psx_native_tail_body' not in cold and 'musttail' not in cold
                    assert not any(x in cold for x in ('psx_mod_function_entry(',
                        'psx_mod_try_function_replacement(', 'psx_cyc_bb_defer_begin('))
                    (work/'selected-owner-proof.c').write_text(cold+'\n'+compiled, encoding='utf-8')
                else:
                    assert 'block_80010700:' in compiled and 'precision_block_' not in compiled
                print('Actual preprocessed '+('native/cold split' if selected else 'portable full-PGXP')+' owner contract PASS', flush=True)
            run([a.cc, *common, *selection, '-c', c, '-o', obj])
            if a.tail_compile and selected:
                tail = [x for x in common if not x.startswith('-DPSX_NATIVE_TAIL_CHAINS=')]
                run([a.cc, *tail, *selection, '-DPSX_NATIVE_TAIL_CHAINS=1',
                     '-c', c, '-o', work/'candidate-tail1-compile.o'])
                print('Same emitted native owner compiles under supported TAIL1; portable TAIL0 products remain the runtime gate', flush=True)
            objects.append(obj)
        for compiler, source, name in (
            (a.cc, ROOT/'runtime/src/psx_ram_geometry.c', 'ram'),
            (a.cxx, ROOT/'runtime/src/pgxp.cpp', 'pgxp'),
            (a.cxx, ROOT/'runtime/tests/pgxp_test_mod_memory.cpp', 'mod-memory'),
            (a.cxx, ROOT/'runtime/tests/pgxp_dataflow_fixture.cpp', 'fixture')):
            obj = work/(name+'.o')
            run([compiler, *common, '-DPSX_PGXP_DATAFLOW='+str(0 if a.regions else 1), '-DPSX_PGXP_OPERATIONS='+str(int(a.operations)), '-DPSX_PGXP_DEMAND_REGIONS='+str(int(a.regions)), '-c', source, '-o', obj]); objects.append(obj)
        if a.regions:
            obj=work/'actual-owner.o'
            run([a.cc,*common,'-c',ROOT/'recompiler/tests/test_static_dispatch_owner.c','-o',obj])
            objects.append(obj)
            # The timing-enabled product uses the exact maintained GTE read.
            # An unrelated aborting linker seam cannot stand in for this path.
            clock=(a.runtime_root/'runtime/src/psx_cycles.c').read_text(encoding='utf-8')
            start=clock.index('void psx_gte_read(CPUState* cpu, uint32_t rt) {')
            end=clock.index('\n}',start)+2
            source=work/'actual-gte-read.c'
            source.write_text('#include "cpu_state.h"\n#include "psx_cycles.h"\n'+clock[start:end]+'\n',encoding='utf-8')
            obj=work/'actual-gte-read.o'
            run([a.cc,*common,'-c',source,'-o',obj]);objects.append(obj)
        if a.geometry_control:
            # Execute the real maintained host/DLL write bridge, not a success
            # stub or a second precision/invalidation representation.
            memory=(a.runtime_root/'runtime/src/memory.c').read_text(encoding='utf-8')
            start=memory.index('void psx_direct_precision_write(')
            end=memory.index('\n}',start)+2
            source=work/'actual-direct-precision-write.c'
            source.write_text('#include "psx_direct_memory.h"\n#include "pgxp.h"\n'+memory[start:end]+'\n',encoding='utf-8')
            obj=work/'actual-direct-precision-write.o'
            run([a.cc,*common,'-DPSX_PGXP_DEMAND_REGIONS=1','-c',source,'-o',obj]);objects.append(obj)
        defined, undefined = nm_symbols(objects)
        provided = library_symbols(a.cc)
        stubs = {s for s in undefined-defined-provided if not s.startswith('__')}
        if any('pgxp_operation' in s or 'pgxp_write_owned' in s or s=='psx_hr_bus_callbacks' or 'pgxp_region_' in s or 'block_edge_materialize' in s or s in ('psx_observation_sample','psx_observation_edge','psx_slice_block_impl','psx_advance_cycles_slow','fixture_regions_owner','fixture_regions_dispatch','psx_gte_read','psx_game_text_native_ok','psx_direct_precision_write') for s in stubs):
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
        if a.compile_only:
            print('COMPILED '+str(result_exe),flush=True)
        else:run([result_exe])
    return 0


if __name__ == '__main__':
    sys.exit(main())
