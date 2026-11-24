#include "cpu_state.h"
#include "psx_cycles.h"
#include "psx_icache.h"
#include "psx_memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native kernel call implementations. Guest stores use the ordinary memory
 * interface, so text-image tracking, dirty pages and runtime mods still see
 * the writes. Unknown calls belong to the strict provider's coverage report. */
static void copy_bytes(CPUState *cpu, uint32_t dest, uint32_t src,
                       uint32_t count, int move) {
    uint32_t src_offset, dest_offset;
    if (move && count &&
        psx_ram_resolve(src, count, &src_offset) &&
        psx_ram_resolve(dest, count, &dest_offset) &&
        dest_offset > src_offset && dest_offset - src_offset < count) {
        for (uint32_t i = count; i != 0; --i)
            cpu->write_byte(dest + i - 1, cpu->read_byte(src + i - 1));
        return;
    }
    while (count && ((dest | src) & 3u)) {
        cpu->write_byte(dest++, cpu->read_byte(src++));
        --count;
    }
    while (count >= 4) {
        cpu->write_word(dest, cpu->read_word(src));
        dest += 4;
        src += 4;
        count -= 4;
    }
    while (count--) cpu->write_byte(dest++, cpu->read_byte(src++));
}

static void fill_bytes(CPUState *cpu, uint32_t dest, uint8_t value, uint32_t count) {
    while (count && (dest & 3u)) {
        cpu->write_byte(dest++, value);
        --count;
    }
    const uint32_t repeated = (uint32_t)value * 0x01010101u;
    while (count >= 4) {
        cpu->write_word(dest, repeated);
        dest += 4;
        count -= 4;
    }
    while (count--) cpu->write_byte(dest++, value);
}

static int service_a0(CPUState *cpu, uint32_t fn) {
    const uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5], a2 = cpu->gpr[6];
    uint32_t result;
    switch (fn) {
    case 0x0e: case 0x0f:  /* abs, labs */
        result = (a0 & 0x80000000u) ? 0u - a0 : a0;
        break;
    case 0x1b:  /* strlen */
        result = 0;
        while (cpu->read_byte(a0 + result)) ++result;
        break;
    case 0x27:  /* bcopy: source first, return source */
        result = a0;
        if (a0 && (int32_t)a2 > 0) copy_bytes(cpu, a1, a0, a2, 0);
        break;
    case 0x28:  /* bzero */
        result = 0;
        if (a0 && (int32_t)a1 > 0) {
            fill_bytes(cpu, a0, 0, a1);
            result = a0;
        }
        break;
    case 0x29: case 0x2d:  /* bcmp, memcmp */
        result = 0;
        if (a0 && a1) {
            for (uint32_t i = 0; i < a2; ++i) {
                int difference = (int)cpu->read_byte(a0 + i) - (int)cpu->read_byte(a1 + i);
                if (difference) { result = (uint32_t)difference; break; }
            }
        }
        break;
    case 0x2a: case 0x2c:  /* memcpy, memmove */
        copy_bytes(cpu, a0, a1, a2, fn == 0x2c);
        result = a0;
        break;
    case 0x2b:  /* memset */
        fill_bytes(cpu, a0, (uint8_t)a1, a2);
        result = a0;
        break;
    case 0x2e:  /* memchr */
        result = 0;
        for (uint32_t i = 0; i < a2; ++i) {
            if (cpu->read_byte(a0 + i) == (uint8_t)a1) {
                result = a0 + i;
                break;
            }
        }
        break;
    case 0x44:  /* FlushCache: invalidate the host cache model directly */
        psx_icache_reset();
        cpu->write_word(0xfffe0130u, 0x0001e988u);
        psx_cpu_charge(40);
        return 1;  /* void function */
    default:
        return 0;
    }
    cpu->gpr[2] = result;
    psx_cpu_charge(24);
    return 1;
}

/* Thread table ABI shared with the scheduler's TCB context save/restore. */
enum { TCB_BYTES = 0xc0, TCB_FREE = 0x1000, TCB_USED = 0x4000 };

static int free_tcb_slot(CPUState *cpu, uint32_t base, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (cpu->read_word(base + i * TCB_BYTES) == TCB_FREE) return (int)i;
    }
    return -1;
}

static int service_thread(CPUState *cpu, uint32_t vector, uint32_t fn) {
    if (!(vector == 0xb0 && (fn == 0x0e || fn == 0x0f)) &&
        !(vector == 0xc0 && fn == 0x05)) return 0;

    uint32_t base = cpu->read_word(0x110);
    uint32_t bytes = cpu->read_word(0x114);
    if (!base || !bytes || (bytes % TCB_BYTES) ||
        !psx_ram_resolve(base, bytes, NULL)) return 0;
    uint32_t count = bytes / TCB_BYTES;
    if (vector == 0xc0) {
        cpu->gpr[2] = (uint32_t)free_tcb_slot(cpu, base, count);
    } else if (fn == 0x0e) {
        int slot = free_tcb_slot(cpu, base, count);
        cpu->gpr[2] = UINT32_MAX;
        if (slot >= 0) {
            uint32_t tcb = base + (uint32_t)slot * TCB_BYTES;
            cpu->write_word(tcb, TCB_USED);
            cpu->write_word(tcb + 4, TCB_FREE);
            cpu->write_word(tcb + 8 + 28 * 4, cpu->gpr[6]);  /* GP */
            cpu->write_word(tcb + 8 + 29 * 4, cpu->gpr[5]);  /* SP */
            cpu->write_word(tcb + 8 + 30 * 4, cpu->gpr[5]);  /* FP */
            cpu->write_word(tcb + 8 + 32 * 4, cpu->gpr[4]);  /* return PC */
            cpu->gpr[2] = 0xff000000u | (uint32_t)slot;
        }
    } else {
        uint32_t slot = cpu->gpr[4] & 0xffffu;
        if (slot >= count) return 0;
        cpu->write_word(base + slot * TCB_BYTES, TCB_FREE);
        cpu->gpr[2] = 1;
    }
    psx_cpu_charge(36);
    return 1;
}

int psx_native_bios_service(CPUState *cpu, uint32_t vector, uint32_t fn) {
    return vector == 0xa0 ? service_a0(cpu, fn) : service_thread(cpu, vector, fn);
}