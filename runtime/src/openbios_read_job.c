/* Build-selected OpenBIOS blocking CD request. All continuation state lives
 * in the original guest frame/registers: no host stack survives a dispatch,
 * IRQ, thread switch, snapshot or rollback. Commands and TestEvent still use
 * the guest A0/B0 vectors, including installed hooks and error callbacks. */
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycles.h"

extern void psx_lle_OpenBIOS_func_1FC03A78(CPUState *cpu);
/* Diagnostic totals only; never read by simulation or serialized as job state.
 * started, finished, failed, deadline waits, elapsed deadline-wait cycles. */
uint64_t g_psx_openbios_read_jobs[5];

static void constant(CPUState *cpu, unsigned reg, uint32_t value) {
    cpu->gpr[reg] = value;
    /* Integer service values have exact halves, no inherited projection. */
    PGXP_ALU(0x3C000000u | (reg << 16u) | (value >> 16u),
             value & 0xFFFF0000u, 0u, 0u);
    PGXP_ALU(0x34000000u | (reg << 21u) | (reg << 16u) | (value & 0xFFFFu),
             value, value & 0xFFFF0000u, 0u);
}

static void call(CPUState *cpu, uint32_t vector, uint32_t selector,
                 uint32_t resume) {
    constant(cpu, 9, selector);
    constant(cpu, 31, resume);
    cpu->pc = vector;
}

static void test(CPUState *cpu, uint32_t handle_word, uint32_t resume) {
    cpu->gpr[4] = cpu->read_word(handle_word);
    PGXP_LOAD(0x8C040000u, handle_word, cpu->gpr[4]);
    call(cpu, 0xB0u, 0x0Bu, resume);
}

static void error(CPUState *cpu, uint32_t code, uint32_t resume) {
    constant(cpu, 4, 0x44u);
    constant(cpu, 5, code);
    call(cpu, 0xA0u, 0xA1u, resume);
}

static int stock_event_poll(CPUState *cpu) {
    /* A custom TestEvent/vector may do work on each poll. Keep calling it
     * without deadline skips. Recheck at each wait; callbacks may patch it. */
    static const uint32_t handler[] = {
        0x3C080000u, 0x250849CCu, 0x00095080u, 0x01485020u,
        0x8D4A0000u, 0x240800B0u, 0x01400008u, 0x00000000u
    };
    if (cpu->read_word(0xB0u) != 0x24082AA0u ||
        cpu->read_word(0xB4u) != 0x01000008u ||
        cpu->read_word(0xB8u) != 0u ||
        (cpu->read_word(0x49F8u) & 0x1FFFFFFFu) != 0x1FC0574Cu)
        return 0;
    for (unsigned i = 0; i < sizeof(handler) / sizeof(handler[0]); ++i)
        if (cpu->read_word(0x2AA0u + i * 4u) != handler[i]) return 0;
    return 1;
}

static void wait(CPUState *cpu, uint32_t resume) {
    cpu->pc = resume;
    /* The deadline accessor describes future events. A delivery already due
     * (including a deferred thread switch) must run before waiting again. */
    if (psx_interrupt_delivery_needed(cpu)) {
        psx_check_interrupts_at(cpu, resume);
        return;
    }
    uint32_t cycles = 32u;
    if (stock_event_poll(cpu)) {
        cycles = psx_idle_cycles_to_next_observable_event();
        /* Bound host maintenance as well as device/audio/IRQ progress. Device
         * service replays masked intermediate events inside this interval. */
        if (cycles > 16384u) cycles = 16384u;
        if (cycles == 0u) cycles = 1u;
        ++g_psx_openbios_read_jobs[3];
        g_psx_openbios_read_jobs[4] += cycles;
    }
    psx_advance_cycles(cycles);
    psx_check_interrupts_at(cpu, resume);
}

static void finish(CPUState *cpu, uint32_t result) {
    ++g_psx_openbios_read_jobs[1];
    if (result == UINT32_MAX) ++g_psx_openbios_read_jobs[2];
    uint32_t sp = cpu->gpr[29];
    cpu->gpr[31] = cpu->read_word(sp + 60u);
    PGXP_LOAD(0x8FBF003Cu, sp + 60u, cpu->gpr[31]);
    if (result == cpu->gpr[16]) {
        cpu->gpr[2] = result;
        PGXP_ALU(0x02001025u, result, result, 0u);
    } else constant(cpu, 2, result);
    for (unsigned i = 0; i < 8; ++i) {
        cpu->gpr[16u + i] = cpu->read_word(sp + 28u + i * 4u);
        PGXP_LOAD(0x8FB0001Cu + (i << 16u) + i * 4u,
                  sp + 28u + i * 4u, cpu->gpr[16u + i]);
    }
    constant(cpu, 29, sp + 64u);
    cpu->pc = cpu->gpr[31];
}

static uint32_t bcd(uint32_t value) { return (value / 10u) * 16u + value % 10u; }

void OpenBIOS_func_1FC03A78(CPUState *cpu) {
    uint32_t pc = cpu->pc & 0x1FFFFFFFu;
    /* Coarse native work charge, separate from elapsed device wait time. */
    psx_cpu_charge(32u);
    switch (pc) {
    case 0u:
    case 0x1FC03A78u: {
        /* Preserve signed-LBA overflow/negative behavior through the LLE
         * route, before creating a native frame. Normal disc LBAs are >=0. */
        if (cpu->gpr[5] > 0x7FFFFF69u) {
            psx_lle_OpenBIOS_func_1FC03A78(cpu);
            return;
        }
        ++g_psx_openbios_read_jobs[0];
        uint32_t lba = cpu->gpr[5] + 150u;
        uint32_t sp = cpu->gpr[29] - 64u;
        cpu->write_word(sp + 60u, cpu->gpr[31]);
        PGXP_STORE(0xAFBF003Cu, sp + 60u, cpu->gpr[31]);
        for (unsigned i = 0; i < 8; ++i) {
            cpu->write_word(sp + 28u + i * 4u, cpu->gpr[16u + i]);
            PGXP_STORE(0xAFB0001Cu + (i << 16u) + i * 4u,
                       sp + 28u + i * 4u, cpu->gpr[16u + i]);
        }
        cpu->write_word(sp + 72u, cpu->gpr[6]);
        PGXP_STORE(0xAFA60048u, sp + 72u, cpu->gpr[6]);
        cpu->write_byte(sp + 16u, (uint8_t)bcd(lba / 4500u));
        cpu->write_byte(sp + 17u, (uint8_t)bcd((lba % 4500u) / 75u));
        cpu->write_byte(sp + 18u, (uint8_t)bcd(lba % 75u));
        constant(cpu, 29, sp);
        cpu->gpr[16] = cpu->gpr[4];
        PGXP_ALU(0x00808025u, cpu->gpr[16], cpu->gpr[4], 0u);
        constant(cpu, 18, 10u);
        constant(cpu, 19, 0xA0u);
        constant(cpu, 20, 0x10000u);
        goto seek;
    }
    case 0x1FC03B68u:
    seek:
        constant(cpu, 17, 99999u);
        goto submit_seek;
    case 0x1FC03B70u:
    submit_seek:
        constant(cpu, 4, cpu->gpr[29] + 16u);
        call(cpu, 0xA0u, 0x78u, 0xBFC03B78u);
        return;
    case 0x1FC03B78u:
        if (!cpu->gpr[2]) {
            constant(cpu, 17, cpu->gpr[17] - 1u);
            if (cpu->gpr[17]) goto submit_seek;
            error(cpu, 0x0Bu, 0xBFC03C38u);
            return;
        }
        constant(cpu, 17, 0xB0u);
        constant(cpu, 21, 0u);
        constant(cpu, 22, 0u);
        /* fall through */
    case 0x1FC03B90u:
        test(cpu, 0x4BE0u, 0xBFC03B9Cu);
        return;
    case 0x1FC03B9Cu:
        if (!cpu->gpr[2]) {
            test(cpu, 0x4BD4u, 0xBFC03C78u);
            return;
        }
        constant(cpu, 17, 99999u);
        constant(cpu, 22, 0xA0u);
        goto submit_read;
    case 0x1FC03C78u:
        if (cpu->gpr[2]) error(cpu, 0x0Cu, 0xBFC03C90u);
        else wait(cpu, 0xBFC03B90u);
        return;
    case 0x1FC03BB4u:
    submit_read:
        cpu->gpr[4] = cpu->gpr[16];
        PGXP_ALU(0x02002025u, cpu->gpr[4], cpu->gpr[16], 0u);
        cpu->gpr[5] = cpu->read_word(cpu->gpr[29] + 72u);
        PGXP_LOAD(0x8FA50048u, cpu->gpr[29] + 72u, cpu->gpr[5]);
        constant(cpu, 6, 0x80u);
        call(cpu, 0xA0u, 0x7Eu, 0xBFC03BC4u);
        return;
    case 0x1FC03BC4u:
        if (!cpu->gpr[2]) {
            constant(cpu, 17, cpu->gpr[17] - 1u);
            if (cpu->gpr[17]) goto submit_read;
            error(cpu, 0x0Cu, 0xBFC03CB0u);
            return;
        }
        constant(cpu, 17, 0xB0u);
        constant(cpu, 22, 0u);
        constant(cpu, 23, 0u);
        /* fall through */
    case 0x1FC03BDCu:
        test(cpu, 0x4BE0u, 0xBFC03BE8u);
        return;
    case 0x1FC03BE8u:
        if (cpu->gpr[2]) finish(cpu, cpu->gpr[16]);
        else test(cpu, 0x4BD4u, 0xBFC03BFCu);
        return;
    case 0x1FC03BFCu:
        if (cpu->gpr[2]) {
            constant(cpu, 18, cpu->gpr[18] - 1u);
            error(cpu, 0x16u, 0xBFC03CD0u);
        } else test(cpu, 0x4BD8u, 0xBFC03C10u);
        return;
    case 0x1FC03C10u:
        if (cpu->gpr[2]) error(cpu, 0x17u, 0xBFC03C90u);
        else wait(cpu, 0xBFC03BDCu);
        return;
    case 0x1FC03CD0u:
        if (cpu->gpr[18]) goto seek;
        error(cpu, 0x0Cu, 0xBFC03CE4u);
        return;
    case 0x1FC03C38u:
    case 0x1FC03C90u:
    case 0x1FC03CB0u:
    case 0x1FC03CE4u:
        finish(cpu, UINT32_MAX);
        return;
    default:
        /* Other original interior entries are not native continuation sites.
         * Their unchanged body remains available (e.g. diagnostic dispatch). */
        psx_lle_OpenBIOS_func_1FC03A78(cpu);
        return;
    }
}
