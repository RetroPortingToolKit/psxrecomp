#include "psx_thread_local.h"
/* Overlay DLLs must update the runtime's store-PC breadcrumb (ABI v24).
 *
 * This TU is an overlay DLL in miniature: it is built with the same defines
 * compile_overlays.py passes, includes the real dispatch preamble, and stores
 * the way generated code does. The "host" hands overlay_init a pointer to its
 * own breadcrumb, like overlay_loader.c does in every build. Before ABI v24 the
 * preamble defined a private g_debug_last_store_pc, so the host value kept
 * naming an older store: fingerprint pc/mmio/sp columns, write attribution and
 * memory.c's PC-keyed store filters all saw the wrong instruction. */
#include <assert.h>
#include <stdint.h>

#include "overlay_dispatch_preamble.c.inc"

/* Verbatim from code_generator.cpp's emitted extern block. Must still compile
 * once cpu_state.h has mapped the name onto the host pointer. */
extern PSX_THREAD_LOCAL uint32_t g_debug_last_store_pc;  /* exact PC of the executing SW/SH/SB — wtrace/readtrace producer attribution (debug_server.c) */

static uint32_t host_last_store_pc;
static uint32_t host_ram_word;

static void host_write_word(uint32_t addr, uint32_t value)
{
    (void)addr;
    /* A store's host-visible side effect runs after the breadcrumb is set;
     * memory.c filters read it here. */
    assert(host_last_store_pc == 0x800657FCu);
    host_ram_word = value;
}

/* Shape of one emitted overlay store (a generated _full_N.c shard). */
static void shard_store(CPUState *cpu)
{
    g_debug_last_store_pc = 0x800657FCu;
    psx_store_cycle_barrier();
    cpu->write_word(cpu->gpr[4] + 304, cpu->gpr[10]);
}

int main(void)
{
    CPUState cpu = {0};
    cpu.write_word = host_write_word;
    cpu.gpr[10] = 0x12345678u;

    /* A store before overlay_init lands in the DLL's private copy. */
    g_debug_last_store_pc = 0x80010000u;
    assert(host_last_store_pc == 0u);

    OverlayCallbacks callbacks = {0};
    callbacks.last_store_pc = &host_last_store_pc;
    overlay_init(&callbacks);
    assert((overlay_abi() & 0xFFFF) >= 24);

    host_last_store_pc = 0xBFC04E90u;  /* last static/BIOS store */
    shard_store(&cpu);
    assert(host_last_store_pc == 0x800657FCu);
    assert(host_ram_word == 0x12345678u);
    assert(g_debug_last_store_pc == 0x800657FCu);

    /* A host that supplies no pointer gets the pre-v24 behaviour back: the
     * DLL keeps its own copy and never touches the host's. */
    callbacks.last_store_pc = 0;
    overlay_init(&callbacks);
    host_last_store_pc = 0xBFC04E90u;
    g_debug_last_store_pc = 0x80065800u;
    assert(host_last_store_pc == 0xBFC04E90u);
    assert(g_debug_last_store_pc == 0x80065800u);
    return 0;
}
