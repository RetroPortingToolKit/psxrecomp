#include "cpu_state.h"
#include "native_bios_services.h"
#include "psx_cycles.h"
#include "psx_icache.h"
#include "psx_memory.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
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

/* The guest's O32 variadic argument slots are 32-bit, independent of host
 * pointer size. Only a validated integer/character conversion reaches host
 * stdio; guest string pointers are read using normal guest memory access. */
static uint32_t console_arg(CPUState *cpu, uint32_t *slot) {
    uint32_t index = (*slot)++;
    return index < 4 ? cpu->gpr[4 + index] :
                       cpu->read_word(cpu->gpr[29] + index * 4u);
}

static int console_string_length(CPUState *cpu, uint32_t address, int precision) {
    uint32_t offset;
    if (!psx_ram_resolve(address, 1, &offset)) return -1;
    uint32_t limit = psx_ram_live_bytes() - offset;
    if (precision >= 0 && (uint32_t)precision < limit) limit = (uint32_t)precision;
    for (uint32_t i = 0; i < limit; ++i) {
        if (!cpu->read_byte(address + i)) return (int)i;
    }
    return precision >= 0 && (uint32_t)precision == limit ? (int)limit : -1;
}

static int console_string(CPUState *cpu, uint32_t address, int width,
                          int precision, int left) {
    int length = address ? console_string_length(cpu, address, precision) : 6;
    if (length < 0) return -1;
    if (!address && precision >= 0 && length > precision) length = precision;
    int padding = width > length ? width - length : 0;
    if (!left) for (int i = 0; i < padding; ++i) fputc(' ', stdout);
    for (int i = 0; i < length; ++i)
        fputc(address ? cpu->read_byte(address + (uint32_t)i) : "<NULL>"[i], stdout);
    if (left) for (int i = 0; i < padding; ++i) fputc(' ', stdout);
    return length + padding;
}

static int console_decimal(CPUState *cpu, uint32_t *address, int *result) {
    int value = 0;
    uint8_t c;
    while ((c = cpu->read_byte(*address)) >= '0' && c <= '9') {
        if (value > (INT_MAX - (c - '0')) / 10) return 0;
        value = value * 10 + c - '0';
        ++*address;
    }
    *result = value;
    return 1;
}

static int console_printf(CPUState *cpu) {
    uint32_t address = cpu->gpr[4], slot = 1;
    int length = console_string_length(cpu, address, -1);
    if (length < 0) return -1;
    uint32_t end = address + (uint32_t)length;
    int written = 0;
    while (address < end) {
        uint8_t c = cpu->read_byte(address++);
        if (c != '%') {
            if (written == INT_MAX || fputc(c, stdout) == EOF) return -1;
            ++written;
            continue;
        }
        if (cpu->read_byte(address) == '%') {
            ++address;
            if (written == INT_MAX || fputc('%', stdout) == EOF) return -1;
            ++written;
            continue;
        }
        char format[48] = "%";
        size_t at = 1;
        int left = 0;
        while (address < end && (c = cpu->read_byte(address)) && strchr("-+ #0", c)) {
            if (at >= 8) return -1;
            if (c == '-') left = 1;
            format[at++] = (char)c;
            ++address;
        }
        int width = 0, precision = -1;
        if (cpu->read_byte(address) == '*') {
            ++address;
            width = (int32_t)console_arg(cpu, &slot);
            if (width == INT_MIN) return -1;
            if (width < 0) {
                width = -width;
                left = 1;
                format[at++] = '-';
            }
        } else if (!console_decimal(cpu, &address, &width)) return -1;
        if (cpu->read_byte(address) == '.') {
            ++address;
            if (cpu->read_byte(address) == '*') {
                ++address;
                precision = (int32_t)console_arg(cpu, &slot);
            } else if (!console_decimal(cpu, &address, &precision)) return -1;
        }
        if ((uint32_t)width > PSX_MAIN_RAM_WINDOW_BYTES ||
            (precision >= 0 && (uint32_t)precision > PSX_MAIN_RAM_WINDOW_BYTES)) return -1;
        int narrow = 0;
        if (cpu->read_byte(address) == 'h') {
            ++address;
            narrow = 16;
            if (cpu->read_byte(address) == 'h') { ++address; narrow = 8; }
        } else if (cpu->read_byte(address) == 'l') {
            ++address; /* PSX long is 32-bit. */
        }
        if (address >= end) return -1;
        c = cpu->read_byte(address++);
        uint32_t value = console_arg(cpu, &slot);
        int count;
        if (c == 's') {
            count = console_string(cpu, value, width, precision, left);
        } else {
            if (!strchr("diuoxXcp", c)) return -1;
            if (c == 'p') {
                if (!width) width = 8;
                format[at++] = '0';
                c = 'x';
            }
            if (width) at += (size_t)snprintf(format + at, sizeof format - at, "%d", width);
            if (precision >= 0)
                at += (size_t)snprintf(format + at, sizeof format - at, ".%d", precision);
            if (at + 1 >= sizeof format) return -1;
            format[at++] = (char)c;
            format[at] = 0;
            if (c == 'd' || c == 'i') {
                int32_t signed_value = narrow == 8 ? (int8_t)value :
                                       narrow == 16 ? (int16_t)value : (int32_t)value;
                count = fprintf(stdout, format, (int)signed_value);
            } else if (c == 'c') {
                count = fprintf(stdout, format, (int)(uint8_t)value);
            } else {
                if (narrow == 8) value &= 0xffu;
                if (narrow == 16) value &= 0xffffu;
                count = fprintf(stdout, format, (unsigned)value);
            }
        }
        if (count < 0 || written > INT_MAX - count) return -1;
        written += count;
    }
    fflush(stdout);
    return written;
}

static int service_console(CPUState *cpu, uint32_t vector, uint32_t fn) {
    if (vector == 0xa0 && fn == 0x3f) {
        int written = console_printf(cpu);
        if (written < 0) return PSX_NATIVE_BIOS_UNSUPPORTED;
        cpu->gpr[2] = (uint32_t)written;
    } else if (vector == 0xb0 && fn == 0x3f) {
        if (console_string(cpu, cpu->gpr[4], 0, -1, 0) < 0)
            return PSX_NATIVE_BIOS_UNSUPPORTED;
        fflush(stdout);
    } else if (vector == 0xb0 && (fn == 0x3d || (fn == 0x3b && cpu->gpr[5] == 1))) {
        fputc((uint8_t)cpu->gpr[4], stdout);
        fflush(stdout);
    } else {
        return PSX_NATIVE_BIOS_UNSUPPORTED;
    }
    psx_cpu_charge(32);
    return PSX_NATIVE_BIOS_RETURN;
}

static int service_a0(CPUState *cpu, uint32_t fn) {
    const uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5], a2 = cpu->gpr[6];
    uint32_t result;
    switch (fn) {
    case 0x0e: case 0x0f:  /* abs, labs */
        result = (a0 & 0x80000000u) ? 0u - a0 : a0;
        break;
    case 0x13: case 0x14: {  /* setjmp / longjmp, SDK JmpBuf register order */
        const unsigned registers[] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
        for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
            if (fn == 0x13) cpu->write_word(a0 + i * 4, cpu->gpr[registers[i]]);
            else cpu->gpr[registers[i]] = cpu->read_word(a0 + i * 4);
        }
        cpu->gpr[2] = fn == 0x13 ? 0 : a1;
        psx_cpu_charge(36);
        if (fn == 0x14) {
            cpu->pc = cpu->gpr[31];
            return PSX_NATIVE_BIOS_TRANSFER;
        }
        return PSX_NATIVE_BIOS_RETURN;
    }
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

/* The IRQ executive keeps ordinary guest HandlerInfo chains in the SDK's
 * existing four-priority table, not in opaque host registration state. */
static int service_irq_chain(CPUState *cpu, uint32_t fn)
{
    uint32_t priority = cpu->gpr[4];
    uint32_t handler = cpu->gpr[5];
    uint32_t table = cpu->read_word(0x100u);
    if (priority >= 4 || !table || !handler || (handler & 3u) ||
        !psx_ram_resolve(table, 32, NULL) || !psx_ram_resolve(handler, 12, NULL))
        return PSX_NATIVE_BIOS_UNSUPPORTED;
    uint32_t link = table + priority * 8;
    if (fn == 3) {
        cpu->write_word(handler, cpu->read_word(link));
        cpu->write_word(link, handler);
        cpu->gpr[2] = 0;
    } else {
        uint32_t node = cpu->read_word(link);
        uint32_t remaining = g_psx_ram_size / 4;
        while (node && node != handler) {
            if (!remaining-- || (node & 3u) || !psx_ram_resolve(node, 12, NULL))
                return PSX_NATIVE_BIOS_UNSUPPORTED;
            link = node;
            node = cpu->read_word(link);
        }
        if (node) cpu->write_word(link, cpu->read_word(node));
        cpu->gpr[2] = node;
    }
    psx_cpu_charge(36);
    return PSX_NATIVE_BIOS_RETURN;
}

int psx_native_bios_service(CPUState *cpu, uint32_t vector, uint32_t fn) {
    if (vector == 0xc0 && (fn == 3 || fn == 4))
        return service_irq_chain(cpu, fn);
    int console = service_console(cpu, vector, fn);
    if (console) return console;

    if (vector == 0xb0 && (fn == 0x56 || fn == 0x57)) {
        cpu->gpr[2] = fn == 0x56 ? PSX_NATIVE_C0_TABLE : PSX_NATIVE_B0_TABLE;
        psx_cpu_charge(24);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xa0 && (fn == 0x56 || fn == 0x72)) {
        const uint32_t events = cpu->read_word(0x120u);
        const uint32_t size = cpu->read_word(0x124u);
        if (size < PSX_NATIVE_BOOT_CD_EVENTS * 0x1cu ||
            !psx_ram_resolve(events, size, NULL)) return PSX_NATIVE_BIOS_UNSUPPORTED;
        cpu->cop0[12] &= ~1u;  /* deinitCDRom enters, and stays in, critical section */
        for (uint32_t i = 0; i < PSX_NATIVE_BOOT_CD_EVENTS; ++i)
            cpu->write_word(events + i * 0x1cu + 4, 0);
        cpu->write_word(PSX_NATIVE_CD_HANDLERS_ACTIVE, 0);
        psx_cpu_charge(40);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xc0 && fn == 0x0d) {
        if (cpu->gpr[4] >= 11) return PSX_NATIVE_BIOS_UNSUPPORTED;
        cpu->write_word(PSX_NATIVE_GENERIC_IRQ_ACK + cpu->gpr[4] * 4, cpu->gpr[5]);
        psx_cpu_charge(24);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xb0 && (fn == 0x18 || fn == 0x19)) {
        cpu->write_word(PSX_NATIVE_EXCEPTION_JUMP_PTR, fn == 0x19 ? cpu->gpr[4] : 0);
        psx_cpu_charge(24);
        return PSX_NATIVE_BIOS_RETURN;
    }
    int card = psx_native_card_service(cpu, vector, fn);
    if (card) return card;
    uint32_t ack = 0;
    uint32_t value = 0;
    if (vector == 0xb0 && fn == 0x5b) {
        ack = PSX_NATIVE_SIO_AUTO_ACK;
        value = cpu->gpr[4];
    } else if (vector == 0xc0 && fn == 0x0a) {
        uint32_t timer = cpu->gpr[4];
        if (timer >= 4) return PSX_NATIVE_BIOS_UNSUPPORTED;
        ack = PSX_NATIVE_TIMER_AUTO_ACK + timer * 4u;
        value = cpu->gpr[5];
    }
    if (ack) {
        cpu->gpr[2] = cpu->read_word(ack);
        cpu->write_word(ack, value);
        psx_cpu_charge(24);
        return PSX_NATIVE_BIOS_RETURN;
    }
    return vector == 0xa0 ? service_a0(cpu, fn) : service_thread(cpu, vector, fn);
}