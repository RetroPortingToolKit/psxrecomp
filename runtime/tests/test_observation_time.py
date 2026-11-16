'''Bounded real clock/edge and build-policy decisions for observation timing.'''
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(argv, ok=True):
    print('RUN', ' '.join(map(str, argv)), flush=True)
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.stdout:
        print(result.stdout, end='', flush=True)
    if (result.returncode == 0) != ok:
        raise AssertionError(f'exit {result.returncode}: {argv}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', required=True)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--ninja', required=True)
    parser.add_argument('--edge-assembly-only', action='store_true')
    parser.add_argument('--clock-only', action='store_true')
    parser.add_argument('--overlay-charge-source', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    inc = root/'runtime/include'
    with tempfile.TemporaryDirectory(prefix='psx-observation-time-') as temp:
        temp = Path(temp)
        (temp/'psx_observation_config.h').write_text(
            '#define PSX_OBSERVATION_WAIT_SITE(pc) ((pc) == 0x8002E6C0u)\n'
            '#define PSX_OBSERVATION_WAIT_SITE_VALUES 0x8002E6C0u\n'
            '#define PSX_OBSERVATION_WAIT_SITE_COUNT 1u\n')
        # Exercise the actual callback without linking the entire overlay loader.
        overlay_source = args.overlay_charge_source or root/'runtime/src/overlay_loader.c'
        overlay_text = overlay_source.read_text(encoding='utf-8')
        maintained_overlay = (root/'runtime/src/overlay_loader.c').read_text(encoding='utf-8')
        def callback(name):
            text = overlay_text if name == 'overlay_cpu_charge' else maintained_overlay
            # Descriptor getter returns a pointer; the other callbacks are void.
            marker = 'static const PSXObservationView *' if name == 'overlay_observation_view' else 'static void '
            start = text.index(marker+name+'(')
            opening = text.index('{', start)
            end, depth = opening + 1, 1
            while depth:
                depth += (text[end] == '{') - (text[end] == '}')
                end += 1
            return text[start:end]
        overlay_charge = temp/'overlay_charge_probe.h'
        overlay_charge.write_text('#include "cpu_state.h"\n'+callback('overlay_cpu_charge')+
            '\n#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME\n'+
            # These typed policy states belong to the loader in the product.
            # The actual descriptor and edge bodies are extracted unchanged.
            'static int s_in_shadow, s_suppress_irq, s_irq_suppress_cdrom_only, s_irq_defer_cdrom;\n'
            'static uint32_t s_irq_budget_cycles, s_irq_ratelimit;\n'
            'static void (*g_overlay_flush_pending_cycles)(void);\n'
            'static int overlay_unit_irq_defer_enabled(void) { return 0; }\n'+
            # Delivery remains the existing fixture's observation seam. This
            # counter only establishes that fallback returns to that boundary.
            'static unsigned s_fixture_legacy_checks;\n'
            'static void overlay_ci_at_wrapper(CPUState *cpu, uint32_t pc) '
            '{ (void)cpu; (void)pc; ++s_fixture_legacy_checks; }\n'+
            callback('overlay_observation_edge')+'\n'+
            callback('overlay_observation_fallback')+'\n'+
            callback('overlay_note_skipped_edge')+'\n'+
            callback('overlay_observation_view')+'\n#endif\n')
        if not args.clock_only:
            probe = temp/'edge_probe.c'
            probe.write_text('#include "cpu_state.h"\nvoid emitted_common_edge(CPUState *cpu, uint32_t pc) { PSX_BLOCK_EDGE(cpu, pc); }\n')
            asm = temp/'edge_probe.s'
            run([args.cc, '-std=c11', '-O2', '-S', '-DPSX_NO_DEBUG_TOOLS',
                 '-DPSX_OBSERVATION_TIME=1', '-I'+str(inc), '-I'+str(temp),
                 str(probe), '-o', str(asm)])
            assembly = asm.read_text()
            assert 'psx_block_edge' not in assembly and 'psx_observation_delivery_pending' not in assembly, assembly
            assert 'psx_observation_edge' in assembly, assembly
            print('PASS O2 emitted edge contains inline policy and only conditional observation fallback')
        if args.edge_assembly_only:
            return
        # Reuse the maintained event-boundary fixture under both policies.
        for opt in ['0', '2']:
            for native in [False, True]:
                exe = temp/f'clock-{opt}-{int(native)}.exe'
                defs = ['-DPSX_OBSERVATION_TIME=1'] if native else []
                run([args.cc, '-std=c11', '-O'+opt, '-DPSX_NO_DEBUG_TOOLS',
                     '-DPSX_TEST_OVERLAY_CHARGE=1', '-include', str(overlay_charge),
                     '-I'+str(inc), '-I'+str(temp), *defs,
                     str(root/'runtime/tests/test_psx_cycle_event_boundaries.c'),
                     str(root/'runtime/src/psx_cycles.c'), '-o', str(exe)])
                run([str(exe)])
        # Reuse the clock fixture with actual root counters for the affected
        # on-demand vs conservative elapsed-time/IRQ differential.
        for opt in ['0', '2']:
            exe = temp/f'clock-real-timers-{opt}.exe'
            run([args.cc, '-std=c11', '-O'+opt, '-DPSX_NO_DEBUG_TOOLS',
                 '-DPSX_ENABLE_BLOCK_CYCLES=1', '-DPSX_OBSERVATION_TIME=1',
                 '-DPSX_CLOCK_REAL_TIMERS=1', '-I'+str(inc), '-I'+str(temp),
                 str(root/'runtime/tests/test_psx_cycle_event_boundaries.c'),
                 str(root/'runtime/src/psx_cycles.c'), str(root/'runtime/src/timers.c'),
                 str(root/'runtime/src/psx_video_timing.c'), '-o', str(exe)])
            run([str(exe)])
        if args.clock_only:
            print('PASS O0/O2 reference/native clocks, actual overlay callback and production timer differential')
            return
        # These compile real bodies; they do not establish runtime IRQ/PCM parity.
        for source in ['interrupts.c', 'interrupts_block_audio.c', 'spu_observation_time.c']:
            run([args.cc, '-std=c11', '-O0', '-fsyntax-only', '-DPSX_NO_DEBUG_TOOLS',
                 '-DPSX_OBSERVATION_TIME=1', '-I'+str(inc), '-I'+str(temp),
                 str(root/'runtime/src'/source)])
        source = temp/'policy'
        source.mkdir()
        (source/'spu.c').write_text('void policy_unused_spu(void) {}\n')
        (source/'main.c').write_text('''#include <stdio.h>
extern const unsigned psx_build_observation_time_model;
int main(void) { printf("%u\\n", psx_build_observation_time_model); return 0; }
''')
        (source/'generated.c').write_text('/* psx-observation-edge-v1 */\n')
        (source/'stale.c').write_text('/* stale generated code */\n')
        (source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(observation_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_observation_time.cmake")
add_library(fixture STATIC spu.c)
target_include_directories(fixture PRIVATE "${{PSXRECOMP_ROOT}}/runtime/include")
psxrecomp_execution_profile(fixture)
psxrecomp_observation_time(fixture GENERATED_SOURCES "${{GENERATED_INPUT}}" WAIT_SITES 0x8002E6C0)
file(GENERATE OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/selection.txt" CONTENT "$<TARGET_PROPERTY:fixture,COMPILE_DEFINITIONS>\\n$<TARGET_PROPERTY:fixture,SOURCES>\\n")
add_executable(probe main.c)
target_link_libraries(probe PRIVATE fixture)
''')
        def configure(name, profile, enabled, generated='generated.c', spans='ON', ok=True):
            build = temp/name
            output = run([args.cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
                          '-DCMAKE_C_COMPILER='+args.cc, '-DCMAKE_MAKE_PROGRAM='+args.ninja,
                          '-DPSX_EXECUTION_PROFILE='+profile, '-DPSX_OBSERVATION_TIME='+enabled,
                          '-DPSX_NATIVE_CPU_SPANS='+spans, '-DGENERATED_INPUT='+generated], ok=ok)
            if not ok:
                assert ('Regenerate game sources' in output or 'requires materialized' in output
                        or 'requires native CPU spans' in output), output
            return build
        manifests = []
        for name, profile, enabled, expected in [('native','ENHANCED','ON',1),
                                                ('reference','REFERENCE','ON',0),
                                                ('disabled','ENHANCED','OFF',0)]:
            build = configure(name, profile, enabled)
            run([args.cmake, '--build', str(build), '--parallel', '1'])
            exe = build/('probe.exe' if os.name == 'nt' else 'probe')
            assert run([str(exe)]).strip() == str(expected)
            selected = (build/'selection.txt').read_text()
            assert ('PSX_OBSERVATION_TIME=1' in selected) == bool(expected), selected
            assert ('spu_observation_time.c' in selected) == bool(expected), selected
            config = (build/'observation-time/fixture/psx_observation_config.h').read_text()
            assert '#define PSX_OBSERVATION_WAIT_SITE_COUNT 1u' in config, config
            assert '#define PSX_OBSERVATION_WAIT_SITE_VALUES 2147673792u' in config, config
            manifests.append(json.loads((build/'psx_execution/fixture/manifest.json').read_text()))
        assert len({m['identity'] for m in manifests}) == 3
        assert all(m['implementations'][0]['name'] == 'machine-time' for m in manifests)
        configure('stale','ENHANCED','ON','stale.c',ok=False)
        configure('missing','ENHANCED','ON','missing.c',ok=False)
        configure('no-spans','ENHANCED','ON',spans='OFF',ok=False)
    print('PASS: O0/O2 canonical/native causality, independent sampling, common edges, MMIO arm, IRQ/frame, wait scope, freeze/replay/restore; real IRQ/SPU syntax; native/reference/off binding and identities')


if __name__ == '__main__':
    main()
