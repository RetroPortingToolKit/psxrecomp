"""Keep local interpreted control flow from bypassing mod entry callbacks."""

from pathlib import Path


def main():
    source = (Path(__file__).resolve().parents[1] /
              'src/dirty_ram_interp.c').read_text(encoding='utf-8')
    start = source.index('static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {')
    source = source[start:]
    static_dispatch = source[source.index('/* B-2: statically-compiled'):]
    static_dispatch = static_dispatch[:static_dispatch.index('/* A-1:')]
    assert static_dispatch.index('g_exec_phase = 3;') < static_dispatch.index(
        'psx_overlay_dispatch(cpu, addr);') < static_dispatch.index(
        'g_exec_phase = previous_phase;') < static_dispatch.index('if (handled) return 1;')
    assert source.count('psx_mod_function_entry(cpu, addr);') == 1
    local_start = source.index('if (allow_local_dirty_flow && target != 0 &&')
    local_end = source.index('current_page = target_phys >> 12;', local_start)
    local = source[local_start:local_end]
    probe_start = local.index('if (psx_overlay_static_can_dispatch(target))')
    probe_end = local.index('#endif', probe_start)
    probe = local[probe_start:probe_end]
    assert 'g_dirty_interp_chain_target = target;' in probe
    assert 'OV_FPLOG_RET1();' in probe
    assert 'psx_overlay_dispatch(cpu' not in probe
    assert local.count('psx_mod_function_entry(cpu, target);') == 1
    assert local.index('overlay_loader_dispatch(cpu, target)') < local.index(
        'psx_mod_function_entry(cpu, target);') < local.index('pc = target;')
    print('interpreter mod entry guards: PASS')


if __name__ == '__main__':
    main()
