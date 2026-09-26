"""A PC in a retail RAM mirror keeps its own address (8 MB mod off or on).

Retail DRAM is 2 MiB mirrored four times across the 8 MiB decode window. Code
running at 0x807xxxxx executes the bytes at 0x801xxxxx, but its PC, the $ra a
jal writes and the EPC an interrupt saves are 0x807xxxxx on hardware. Folding
the PC (the 8 MB branch once rewrote `addr` at the top of dirty_ram_dispatch
and in the emitted game lookups) makes compiled 0x801xxxxx bodies run for it
and hands the guest folded return addresses.

The contract (runtime/include/psx_memory.h): CODE identity is the segment-
stripped PC, never mirror-folded; BYTE identity (fetch, dirty/overlay page
checks) folds through the live geometry. The behavioural half lives in
recompiler/tests/test_kuseg_dispatch_lookup.py (mirror PCs never resolve to a
compiled body, in either geometry, against the real psx_ram_geometry.c) and
runtime/tests/test_psx_ram_runtime_map.c (mirror bytes fold only on retail).
This guard pins the interpreter side, which cannot run outside the runtime.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
INTERP = (ROOT / 'runtime/src/dirty_ram_interp.c').read_text(encoding='utf-8')
MEMORY = (ROOT / 'runtime/src/memory.c').read_text(encoding='utf-8')
EMITTER = (ROOT / 'recompiler/src/full_function_emitter.cpp').read_text(encoding='utf-8')
GAME = (ROOT / 'recompiler/src/main_psx.cpp').read_text(encoding='utf-8')


def body(source, signature):
    start = source.index(signature)
    depth, pos = 0, source.index('{', start)
    while True:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[start:pos]
        pos += 1


def main():
    # No code-address canonicalizer exists anywhere: nothing can fold a PC.
    for path in list((ROOT / 'runtime/src').glob('*.c*')) + \
            list((ROOT / 'runtime/include').glob('*.h')) + \
            list((ROOT / 'recompiler/src').glob('*.cpp')):
        text = path.read_text(encoding='utf-8', errors='replace')
        assert 'canon_code_addr' not in text, f'{path.name}: PC canonicalizer is back'

    # dirty_ram_dispatch runs at exactly the PC it was handed.
    outer = body(INTERP, 'int dirty_ram_dispatch(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {')
    assert not re.search(r'\baddr\s*=[^=]', outer), 'dirty_ram_dispatch rewrites addr'
    assert not re.search(r'\bstop_addr\s*=[^=]', outer), 'dirty_ram_dispatch rewrites stop_addr'
    assert 'dirty_ram_dispatch_inner(cpu, addr, stop_addr)' in outer

    # Inside, code identity and byte identity are separate names.
    inner = body(INTERP, 'static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {')
    assert 'uint32_t phys = addr & 0x1FFFFFFFu;' in inner
    assert 'const uint32_t ram_phys = psx_ram_map_read(phys);' in inner
    assert 'uint32_t pc = addr;' in inner, 'the interpreted PC must start at addr'
    assert 'phys_is_overlay_region(ram_phys)' in inner
    assert 'phys_is_overlay_flow_region(ram_phys)' in inner
    assert 'dirty_ram_mark_executable_range(ram_phys, 4u)' in inner
    local = body(INTERP, 'static int is_local_dirty_target(uint32_t target) {')
    assert 'psx_ram_map_read(target)' in local
    fetch = body(INTERP, 'static inline uint32_t fetch_word(uint32_t phys) {')
    assert 'phys = psx_ram_map_read(phys);' in fetch, 'fetch must read the folded bytes'
    dirty = body(MEMORY, 'int dirty_ram_is_dirty(uint32_t phys) {')
    assert dirty.index('phys = psx_ram_map_read(phys);') < dirty.index('if (phys >= RAM_LIVE) return 0;')

    # The emitted dispatchers hand the unmodified PC to every lookup.
    assert 'found = dirty_ram_dispatch(cpu, addr, stop_addr);' in EMITTER
    assert 'uint32_t game_phys = addr & 0x1FFFFFFFu;' in EMITTER
    find = GAME[GAME.index('static const PsxGameDispatchEntry* psx_game_find_entry'):]
    find = find[:find.index('ds << "}\\n\\n";')]
    assert 'const uint32_t want = addr & 0x1FFFFFFFu;' in find
    assert 'psx_ram' not in find, 'game lookup must not fold mirror PCs'
    print('mirror PC preserved guards: PASS')


if __name__ == '__main__':
    main()
