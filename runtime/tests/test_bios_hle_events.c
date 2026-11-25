#include "bios_hle.h"
#include "native_bios_services.h"
#include "cpu_state.h"
#include "psx_bios_image.h"
#include "psx_cycles.h"
#include "psx_memory.h"
#include "sio.h"
#include "memcard.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    EVENT_BASE = 0x2000,
    EVENT_SIZE = 0x1c,
    EVENT_CLASS = 0x10,
    OUTER_SPEC = 4,
    NESTED_SPEC = 8,
    CALLBACK_FIRST = 0x10000,
    CALLBACK_SECOND = 0x10004,
    CALLBACK_NESTED = 0x10008,
    CALLBACK_OVERRIDE = 0x10014,
    CALLBACK_CARD_FIND = 0x10024,
    CALLBACK_CARD_COMPLETE = 0x10028,
    IRQ_VERIFY_A = 0x10018,
    IRQ_VERIFY_B = 0x1001c,
    IRQ_HANDLER = 0x10020,
    CALLER_RESUME = 0x1000c,
    NESTED_RESUME = 0x10010,
    RETAIL_RESUME = 0x1bfc,
    ENABLED = 0x2000,
    READY = 0x4000,
    MODE_CALLBACK = 0x1000,
    MODE_MARK = 0x2000,
};

static uint32_t ram[0x10000 / sizeof(uint32_t)];
static uint32_t calls[4];
static size_t call_count;
PsxBiosImageInfo psx_bios_image;
uint32_t g_psx_ram_size = sizeof ram;
uint32_t g_psx_ram_mask = sizeof ram - 1;
static unsigned cache_resets;
static int irq_active;
static uint32_t expected_saved_v0, last_thread_switch;
static uint32_t test_istat, test_imask;
static uint32_t test_timers[3][3];
static uint32_t test_gpu_status, test_gpu_data, test_gpu_control;
static unsigned test_gpu_writes;
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
static uint16_t test_sio_mode, test_sio_ctrl, test_sio_baud;
static uint8_t test_card[2][MEMCARD_SECTORS][MEMCARD_SECTOR_SIZE];
static int test_card_present[2];
static uint8_t test_card_flag[2];
static unsigned card_flushes;
static int card_flush_result;
static unsigned card_find_overrides;
static unsigned card_file_completions;
static uint32_t expected_card_device, expected_card_spec;
static void (*card_replaced_hook)(void);
static jmp_buf irq_escape;
static CPUState irq_cpu;
static unsigned wait_checks;
static uint32_t wait_delivery;
void psx_icache_reset(void) { ++cache_resets; }
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle = UINT64_MAX;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
int g_exec_phase, g_event_step_conservative, g_ls_replay_active, psx_in_device_service;

#define CHECK(test) do { \
    if (!(test)) { fprintf(stderr, "B0 event fixture line %d: %s\n", __LINE__, #test); exit(1); } \
} while (0)

uint32_t psx_idle_cycles_to_next_observable_event(void) { return 64; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t pc) {
    CHECK(pc == (0x80000000u | PSX_NATIVE_WAIT_EVENT_RESUME));
    CHECK(wait_delivery && cpu->gpr[3] == wait_delivery);
    cpu->write_word(wait_delivery + 4, READY);
    ++wait_checks;
}

void sio_set_cards_replaced_hook(void (*hook)(void)) { card_replaced_hook = hook; }
uint8_t sio_card_flag(int port) { return test_card_flag[port]; }
void sio_card_mark_written(int port) { test_card_flag[port] = 0; }
int memcard_is_present(int port) { return test_card_present[port]; }
int memcard_read_sector(int port, int sector, uint8_t *buffer) {
    if (!test_card_present[port] || sector < 0 || sector >= MEMCARD_SECTORS) return -1;
    memcpy(buffer, test_card[port][sector], MEMCARD_SECTOR_SIZE);
    return 0;
}
int memcard_write_sector(int port, int sector, const uint8_t *buffer) {
    if (!test_card_present[port] || sector < 0 || sector >= MEMCARD_SECTORS) return -1;
    memcpy(test_card[port][sector], buffer, MEMCARD_SECTOR_SIZE);
    return 0;
}
void memcard_flush(int port) { CHECK(test_card_present[port]); ++card_flushes; }
int memcard_last_flush_result(int port) { CHECK(test_card_present[port]); return card_flush_result; }

int fntrace_is_game_started(void) { return 1; }
int psx_get_in_exception(void) { return irq_active; }
void psx_restore_state_escape(void) { CHECK(irq_active); longjmp(irq_escape, 2); }
void psx_scheduler_resume_at(uint32_t pc) { (void)pc; CHECK(0); }
uint32_t psx_sched_current_tcb(CPUState *cpu) {
    if (irq_active) CHECK(cpu == &irq_cpu);
    return 0x1000;
}
void psx_sched_save_context(CPUState *cpu, uint32_t tcb, uint32_t pc) {
    CHECK(irq_active && cpu == &irq_cpu && tcb == 0x1000 && pc == CALLER_RESUME);
    CHECK(cpu->cop0[12] == 0x40000401u);
    if (expected_saved_v0) CHECK(cpu->gpr[2] == expected_saved_v0);
}
int psx_syscall(CPUState *cpu, uint32_t code) {
    if (!irq_active) {
        CHECK(code == 0 && cpu->gpr[4] == 3 && cpu->gpr[31] == CALLER_RESUME);
        last_thread_switch = cpu->gpr[5];
        return 0;
    }
    CHECK(irq_active && cpu == &irq_cpu && code == 0 &&
          cpu->gpr[4] == 3 && cpu->gpr[5] == 0x1000);
    longjmp(irq_escape, 1);
}

uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_devices_service_to_now(void) { CHECK(0); }
void psx_advance_cycles_slow(uint32_t cycles) { (void)cycles; CHECK(0); }

static uint32_t read_word(uint32_t address)
{
    if (address == 0x1f801814u) return test_gpu_status;
    if (address == 0x1f8010a8u) return 0;
    if (address == 0x1f801070u) return test_istat;
    if (address == 0x1f801074u) return test_imask;
    if (address >= 0x1f801100u && address < 0x1f801130u &&
        ((address - 0x1f801100u) & 15u) <= 8u)
        return test_timers[(address - 0x1f801100u) / 16][(address & 15u) / 4];
    address &= 0x1fffffff;
    CHECK(!(address & 3) && address < sizeof ram);
    return ram[address / 4];
}

static void write_word(uint32_t address, uint32_t value)
{
    if (address == 0x1f801810u) {
        test_gpu_data = value;
        ++test_gpu_writes;
        return;
    }
    if (address == 0x1f801814u) {
        test_gpu_control = value;
        return;
    }
    if (address == 0x1f801070u) { test_istat &= value; return; }
    if (address == 0x1f801074u) { test_imask = value; return; }
    if (address >= 0x1f801100u && address < 0x1f801130u &&
        ((address - 0x1f801100u) & 15u) <= 8u) {
        test_timers[(address - 0x1f801100u) / 16][(address & 15u) / 4] = value;
        return;
    }
    if (address == 0xfffe0130u) { CHECK(value == 0x0001e988u); return; }
    address &= 0x1fffffff;
    CHECK(!(address & 3) && address < sizeof ram);
    ram[address / 4] = value;
}

static uint8_t read_byte(uint32_t address)
{
    return (uint8_t)(read_word(address & ~3u) >> ((address & 3u) * 8));
}
static void write_byte(uint32_t address, uint8_t value)
{
    const unsigned shift = (address & 3u) * 8;
    uint32_t word = read_word(address & ~3u);
    write_word(address & ~3u, (word & ~(0xffu << shift)) | ((uint32_t)value << shift));
}

static void write_half(uint32_t address, uint16_t value)
{
    if (address == 0x1f801048u) { test_sio_mode = value; return; }
    if (address == 0x1f80104au) { test_sio_ctrl = value; return; }
    if (address == 0x1f80104eu) { test_sio_baud = value; return; }
    write_byte(address, (uint8_t)value);
    write_byte(address + 1, (uint8_t)(value >> 8));
}

static int deliver(CPUState *cpu, uint32_t spec, uint32_t caller_resume)
{
    cpu->gpr[4] = EVENT_CLASS;
    cpu->gpr[5] = spec;
    cpu->gpr[9] = 0x07;
    cpu->gpr[31] = caller_resume;
    return g_psx_bios_hle_hook(cpu, 0xb0);
}

void psx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t return_address)
{
    const PsxNativeBiosTable *table = &psx_native_bios_tables[0];
    uint32_t first = psx_native_bios_stub(table, 0x65);
    uint32_t last = psx_native_bios_stub(table, 0x6d);
    if (addr == CALLBACK_CARD_COMPLETE) {
        CHECK(return_address == PSX_NATIVE_IRQ_CALLBACK_LINK);
        if (cpu->gpr[4] != 0xf0000011u || cpu->gpr[5] != expected_card_spec)
            fprintf(stderr, "card completion %u: class=%08x spec=%08x expected=%08x device=%08x\n",
                    card_file_completions, cpu->gpr[4], cpu->gpr[5], expected_card_spec,
                    read_word(PSX_NATIVE_CARD_LAST_DEVICE));
        CHECK(cpu->gpr[4] == 0xf0000011u && cpu->gpr[5] == expected_card_spec);
        CHECK(read_word(PSX_NATIVE_CARD_LAST_DEVICE) == expected_card_device);
        CHECK(read_word(PSX_NATIVE_CARD_STATUS + (expected_card_device >> 4) * 4) & 1u);
        ++card_file_completions;
        /* A real guest callback can overwrite volatile argument registers. */
        cpu->gpr[4] = cpu->gpr[5] = cpu->gpr[6] = 0xffffffffu;
        return;
    }
    if (addr == CALLBACK_CARD_FIND) {
        CHECK(return_address == PSX_NATIVE_IRQ_CALLBACK_LINK);
        CHECK(read_word(cpu->gpr[4]) == 1);
        ++card_find_overrides;
        uint32_t original = psx_native_bios_stub(table, 0x69);
        write_word(PSX_NATIVE_DEVICE_TABLE + 0x34, original);
        CHECK(psx_native_card_service(cpu, 0xa0, 0x69));
        return;
    }
    if (addr >= first && addr <= last && (addr - first) % 12 == 0) {
        CHECK(return_address == PSX_NATIVE_IRQ_CALLBACK_LINK);
        CHECK(g_psx_bios_hle_hook(cpu, addr & 0x1fffffffu));
        return;
    }
    if (addr == IRQ_VERIFY_A || addr == IRQ_VERIFY_B || addr == IRQ_HANDLER) {
        CHECK(irq_active && cpu == &irq_cpu && return_address == PSX_NATIVE_IRQ_CALLBACK_LINK &&
              cpu->gpr[31] == PSX_NATIVE_IRQ_CALLBACK_LINK);
        CHECK(call_count < sizeof calls / sizeof calls[0]);
        calls[call_count++] = addr;
        if (addr == IRQ_HANDLER) {
            CHECK(cpu->gpr[4] == 7 && read_word(EVENT_BASE + 4) == ENABLED);
        } else {
            cpu->gpr[2] = addr == IRQ_VERIFY_A ? 7 : 0;
        }
        return;
    }
    if (addr == CALLBACK_OVERRIDE) {
        CHECK(return_address == CALLER_RESUME && cpu->gpr[31] == CALLER_RESUME);
        cpu->gpr[2] = 0x7bad;
        return;
    }

    CHECK(call_count < sizeof calls / sizeof calls[0]);
    calls[call_count++] = addr;
    uint32_t expected = addr == CALLBACK_NESTED ? NESTED_RESUME : CALLER_RESUME;
    if (psx_bios_image.deliver_event_ret) expected = RETAIL_RESUME;
    CHECK(return_address == expected && cpu->gpr[31] == expected);
    if (addr == CALLBACK_FIRST) {
        CHECK(deliver(cpu, NESTED_SPEC, NESTED_RESUME));
        CHECK(cpu->gpr[31] == NESTED_RESUME);
    }
}

static void add_event(unsigned slot, uint32_t spec, uint32_t mode, uint32_t target)
{
    uint32_t addr = EVENT_BASE + EVENT_SIZE * slot;
    write_word(addr + 0x00, EVENT_CLASS);
    write_word(addr + 0x04, ENABLED);
    write_word(addr + 0x08, spec);
    write_word(addr + 0x0c, mode);
    write_word(addr + 0x10, target);
}

static void check_delivery(uint32_t firmware_return)
{
    memset(ram, 0, sizeof ram);
    call_count = 0;
    psx_bios_image.deliver_event_ret = firmware_return;
    psx_bios_hle_configure(1, 0);
    CHECK(psx_bios_hle_enabled() && g_psx_bios_hle_hook);
    write_word(0x120, EVENT_BASE);
    write_word(0x124, EVENT_SIZE * 5);
    add_event(0, OUTER_SPEC, MODE_CALLBACK, CALLBACK_FIRST);
    add_event(1, OUTER_SPEC, MODE_MARK, 0);
    add_event(2, OUTER_SPEC, MODE_CALLBACK, CALLBACK_SECOND);
    add_event(3, NESTED_SPEC, MODE_CALLBACK, CALLBACK_NESTED);
    add_event(4, OUTER_SPEC, MODE_CALLBACK, 0);

    CPUState cpu = {0};
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    if (!firmware_return) {
        CHECK(!deliver(&cpu, OUTER_SPEC, 0));
        CHECK(!call_count && read_word(EVENT_BASE + EVENT_SIZE + 0x04) == ENABLED);
    }
    CHECK(deliver(&cpu, OUTER_SPEC, CALLER_RESUME));
    CHECK(call_count == 3 && calls[0] == CALLBACK_FIRST &&
          calls[1] == CALLBACK_NESTED && calls[2] == CALLBACK_SECOND);
    CHECK(cpu.gpr[31] == CALLER_RESUME);
    CHECK(read_word(EVENT_BASE + EVENT_SIZE + 0x04) == READY);
}

static uint32_t native_call(CPUState *cpu, uint32_t vector, uint32_t fn,
                            uint32_t a0, uint32_t a1, uint32_t a2)
{
    cpu->gpr[9] = fn;
    cpu->gpr[4] = a0;
    cpu->gpr[5] = a1;
    cpu->gpr[6] = a2;
    cpu->gpr[31] = CALLER_RESUME;
    CHECK(g_psx_bios_hle_hook(cpu, vector));
    return cpu->gpr[2];
}

static void check_native_calls(void)
{
    memset(ram, 0, sizeof ram);
    psx_bios_hle_configure_native();
    psx_native_bios_init_tables(write_word);
    CHECK(strcmp(psx_bios_hle_backend_name(), "HLE (native kernel)") == 0);
    CPUState cpu = {0};
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    cpu.read_byte = read_byte;
    cpu.write_byte = write_byte;
    cpu.write_half = write_half;

    write_word(0x110, 0x1000);
    write_word(0x114, 2 * 0xc0);
    write_word(0x1000, 0x4000);
    write_word(0x10c0, 0x1000);
    CHECK(native_call(&cpu, 0xc0, 0x05, 0, 0, 0) == 1);
    CHECK(native_call(&cpu, 0xb0, 0x0e, 0x80012340, 0x801f0000, 0x80040000) == 0xff000001);
    CHECK(read_word(0x10c0) == 0x4000 && read_word(0x10c4) == 0x1000);
    CHECK(read_word(0x10c0 + 8 + 28 * 4) == 0x80040000);
    CHECK(read_word(0x10c0 + 8 + 29 * 4) == 0x801f0000);
    CHECK(read_word(0x10c0 + 8 + 30 * 4) == 0x801f0000);
    CHECK(read_word(0x10c0 + 8 + 32 * 4) == 0x80012340);
    CHECK(native_call(&cpu, 0xb0, 0x0e, 0, 0, 0) == UINT32_MAX);
    CHECK(native_call(&cpu, 0xb0, 0x10, 0xff000000, 0, 0) == 1);
    CHECK(!last_thread_switch);
    CHECK(native_call(&cpu, 0xb0, 0x10, 0xff000001, 0, 0) == 1);
    CHECK(last_thread_switch == 0x10c0);
    CHECK(native_call(&cpu, 0xb0, 0x10, 0xff000002, 0, 0) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x0f, 0xff000001, 0, 0) == 1);
    CHECK(native_call(&cpu, 0xc0, 0x05, 0, 0, 0) == 1);

    CHECK(native_call(&cpu, 0xb0, 0x56, 0, 0, 0) == PSX_NATIVE_C0_TABLE);
    CHECK(native_call(&cpu, 0xb0, 0x57, 0, 0, 0) == PSX_NATIVE_B0_TABLE);
    CHECK(read_word(PSX_NATIVE_C0_TABLE + 6 * 4) == (0x80000000u | PSX_NATIVE_EXCEPTION_ENTRY));
    CHECK(read_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x38) ==
          (0x08000000u | (PSX_NATIVE_EXCEPTION_PATCH_RESUME >> 2)));
    CHECK(read_word(PSX_NATIVE_EXCEPTION_PATCH_RESUME) == PSX_NATIVE_EXCEPTION_MARKER);
    const PsxNativeBiosTable *a0 = &psx_native_bios_tables[0];
    const uint32_t fill_stub = psx_native_bios_stub(a0, 0x2b);
    write_word(PSX_NATIVE_A0_TABLE + 0x2b * 4, CALLBACK_OVERRIDE);
    CHECK(native_call(&cpu, 0xa0, 0x2b, 0x1801, 0x51, 9) == 0x7bad);
    CHECK(read_byte(0x1801) == 0);
    CHECK(g_psx_bios_hle_hook(&cpu, fill_stub & 0x1fffffffu));
    CHECK(cpu.gpr[2] == 0x1801 && read_byte(0x1801) == 0x51);
    write_word(PSX_NATIVE_A0_TABLE + 0x2b * 4, fill_stub);
    write_word(fill_stub, 0x2409002au);
    CHECK(!g_psx_bios_hle_hook(&cpu, fill_stub & 0x1fffffffu));
    write_word(fill_stub, psx_native_bios_stub_word(a0, 0x2b, 0));
    write_word(0xa0, 0);
    CHECK(!g_psx_bios_hle_hook(&cpu, 0xa0));
    write_word(0xa0, psx_native_bios_vector_word(a0, 0));
    CHECK(native_call(&cpu, 0xa0, 0x2b, 0x1801, 0x51, 9) == 0x1801);
    CHECK(read_byte(0x1800) == 0 && read_byte(0x180a) == 0);
    for (unsigned i = 1; i <= 9; ++i) CHECK(read_byte(0x1800 + i) == 0x51);
    CHECK(native_call(&cpu, 0xa0, 0x1b, 0x1801, 0, 0) == 9);
    CHECK(native_call(&cpu, 0xa0, 0x2a, 0x1821, 0x1801, 9) == 0x1821);
    CHECK(native_call(&cpu, 0xa0, 0x29, 0x1821, 0x1801, 9) == 0);
    for (unsigned i = 0; i < 8; ++i) write_byte(0x1840 + i, (uint8_t)i);
    CHECK(native_call(&cpu, 0xa0, 0x2c, 0x1842, 0x1840, 6) == 0x1842);
    for (unsigned i = 0; i < 6; ++i) CHECK(read_byte(0x1842 + i) == i);
    for (unsigned i = 16; i <= 23; ++i) cpu.gpr[i] = 0x9000 + i;
    cpu.gpr[28] = 0x80040000;
    cpu.gpr[29] = 0x801fe000;
    cpu.gpr[30] = 0x801fdff0;
    CHECK(native_call(&cpu, 0xa0, 0x13, 0x1900, 0, 0) == 0);
    CHECK(read_word(0x1900) == CALLER_RESUME && read_word(0x192c) == 0x80040000);
    memset(cpu.gpr, 0, sizeof cpu.gpr);
    cpu.gpr[4] = 0x1900;
    cpu.gpr[5] = 5;
    CHECK(psx_native_bios_service(&cpu, 0xa0, 0x14) == PSX_NATIVE_BIOS_TRANSFER);
    CHECK(cpu.pc == CALLER_RESUME && cpu.gpr[2] == 5 &&
          cpu.gpr[29] == 0x801fe000 && cpu.gpr[30] == 0x801fdff0);
    for (unsigned i = 16; i <= 23; ++i) CHECK(cpu.gpr[i] == 0x9000 + i);
    native_call(&cpu, 0xb0, 0x19, 0x1900, 0, 0);
    CHECK(read_word(PSX_NATIVE_EXCEPTION_JUMP_PTR) == 0x1900);
    native_call(&cpu, 0xb0, 0x18, 0, 0, 0);
    CHECK(read_word(PSX_NATIVE_EXCEPTION_JUMP_PTR) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x5b, 1, 0, 0) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x5b, 0, 0, 0) == 1);
    write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 1);
    CHECK(native_call(&cpu, 0xc0, 0x0a, 3, 0, 0) == 1);
    CHECK(native_call(&cpu, 0xc0, 0x0a, 3, 1, 0) == 0);
    CHECK(read_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4) == 1);
    write_word(0x120, EVENT_BASE);
    write_word(0x124, 6 * EVENT_SIZE);
    for (unsigned i = 0; i < 6; ++i) add_event(i, 2, MODE_MARK, 0);
    write_word(PSX_NATIVE_CD_HANDLERS_ACTIVE, 1);
    cpu.cop0[12] = 0x40000401u;
    native_call(&cpu, 0xa0, 0x72, 0, 0, 0);
    CHECK(!(cpu.cop0[12] & 1u) && read_word(PSX_NATIVE_CD_HANDLERS_ACTIVE) == 0);
    for (unsigned i = 0; i < 5; ++i) CHECK(read_word(EVENT_BASE + i * EVENT_SIZE + 4) == 0);
    CHECK(read_word(EVENT_BASE + 5 * EVENT_SIZE + 4) == ENABLED);
    const char format[] = "%08x %d %s %c %%";
    const char word[] = "ok";
    for (unsigned i = 0; i < sizeof format; ++i) write_byte(0x1a00 + i, (uint8_t)format[i]);
    for (unsigned i = 0; i < sizeof word; ++i) write_byte(0x1a40 + i, (uint8_t)word[i]);
    CHECK(native_call(&cpu, 0xa0, 0x19, 0x1b00, 0x1a40, 0) == 0x1b00);
    CHECK(read_byte(0x1b00) == 'o' && read_byte(0x1b01) == 'k' && !read_byte(0x1b02));
    write_byte(0x1b01, 'z');
    CHECK(native_call(&cpu, 0xa0, 0x18, 0x1b00, 0x1a40, 1) == 0);
    CHECK((int32_t)native_call(&cpu, 0xa0, 0x18, 0x1b00, 0x1a40, 2) > 0);
    CHECK((int32_t)native_call(&cpu, 0xa0, 0x17, 0x1a40, 0x1b00, 0) < 0);
    write_word(0x1b00, UINT32_MAX);
    CHECK(native_call(&cpu, 0xa0, 0x1a, 0x1b00, 0x1a40, 4) == 0x1b00);
    CHECK(read_word(0x1b00) == 0x00006b6fu);
    cpu.gpr[7] = 0x1a40;
    cpu.gpr[29] = 0x3000;
    write_word(0x3010, '!');
    CHECK(native_call(&cpu, 0xa0, 0x3f, 0x1a00, 0x2a, (uint32_t)-7) == 18);
    native_call(&cpu, 0xb0, 0x3f, 0x1a40, 0, 0);
    CHECK(native_call(&cpu, 0xb0, 0x35, 1, 0x1a40, 2) == 2);
    CHECK(native_call(&cpu, 0xa0, 0x03, 1, 0x1a40, 0) == 0);
    write_word(PSX_NATIVE_CARD_FASTTRACK_SCRATCH + 0x28, 0x12345678);
    write_word(0xdf80, 0x87654321);
    psx_native_card_bind(&cpu);
    CHECK(card_replaced_hook);
    card_replaced_hook();
    for (unsigned i = 0; i < 64; i += 4)
        CHECK(read_word(PSX_NATIVE_CARD_REQUEST + i) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x4a, 0, 0, 0) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x4a, 0, 0, 0) == 1);
    CHECK(native_call(&cpu, 0xb0, 0x5c, 1, 0, 0) == 1);
    CHECK(read_word(PSX_NATIVE_CARD_FASTTRACK_SCRATCH + 0x28) == 0x12345678 &&
          read_word(0xdf80) == 0x87654321);
    test_imask = 0;
    CHECK(native_call(&cpu, 0xb0, 0x4b, 0, 0, 0) == 1);
    CHECK(test_sio_mode == 13 && test_sio_baud == 136 && test_sio_ctrl == 0 &&
          test_imask == 1 && read_word(PSX_NATIVE_TIMER_AUTO_ACK + 12) == 0);
    test_istat = 1;
    psx_native_card_vblank(&cpu);
    CHECK(test_istat == 0 && read_word(PSX_NATIVE_CARD_PORT) == 1);
    /* Real backup-unit scan, async byte transfer, shared event ABI and
     * backing-replacement invalidation. */
    test_card_present[0] = 1;
    test_card_present[1] = 0;
    test_card_flag[0] = 8;
    test_card[0][0][0] = 'M';
    test_card[0][0][1] = 'C';
    test_card[0][0][127] = 'M' ^ 'C';
    psx_native_card_bind(&cpu);
    CHECK(native_call(&cpu, 0xa0, 0x70, 0, 0, 0) == 0);
    CHECK(read_word(PSX_NATIVE_CARD_VOLUME_VALID) == 1 &&
          read_word(PSX_NATIVE_CARD_VOLUME_VALID + 4) == 0);
    CHECK(card_flushes == 1 && test_card_flag[0] == 0 &&
          !memcmp(test_card[0][0], test_card[0][63], MEMCARD_SECTOR_SIZE));
    write_word(0x124, 4 * EVENT_SIZE);
    const uint32_t specs[] = {4, 0x8000, 4, 0x8000};
    for (unsigned i = 0; i < 4; ++i) {
        add_event(i, specs[i], MODE_MARK, 0);
        write_word(EVENT_BASE + i * EVENT_SIZE, i < 2 ? 0xf0000011u : 0xf4000001u);
    }
    for (unsigned i = 0; i < MEMCARD_SECTOR_SIZE; ++i)
        write_byte(0x3000 + i, (uint8_t)(i ^ 0x5a));
    CHECK(native_call(&cpu, 0xb0, 0x4e, 0, 64, 0x3000) == 1);
    CHECK(native_call(&cpu, 0xb0, 0x4e, 0, 65, 0x3000) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x5c, 0, 0, 0) == 4);
    CHECK(card_flushes == 1);
    psx_native_card_vblank(&cpu); /* port 0 */
    CHECK(card_flushes == 2 && read_word(EVENT_BASE + 4) == READY);
    for (unsigned i = 0; i < MEMCARD_SECTOR_SIZE; ++i)
        CHECK(test_card[0][64][i] == (uint8_t)(i ^ 0x5a));
    CHECK(native_call(&cpu, 0xb0, 0x4f, 0, 64, 0x3100) == 1);
    psx_native_card_vblank(&cpu); /* port 1: no operation */
    CHECK(read_word(EVENT_BASE + 4) == ENABLED);
    psx_native_card_vblank(&cpu); /* port 0: read */
    for (unsigned i = 0; i < MEMCARD_SECTOR_SIZE; ++i)
        CHECK(read_byte(0x3100 + i) == (uint8_t)(i ^ 0x5a));
    CHECK(read_word(EVENT_BASE + 4) == READY);
    CHECK(native_call(&cpu, 0xa0, 0xab, 0x10, 0, 0) == 1);
    psx_native_card_vblank(&cpu);
    CHECK(read_word(EVENT_BASE + EVENT_SIZE + 4) == READY &&
          read_word(EVENT_BASE + 3 * EVENT_SIZE + 4) == READY);
    CHECK(native_call(&cpu, 0xb0, 0x5c, 1, 0, 0) == 0x21);
    CHECK(native_call(&cpu, 0xb0, 0x58, 0, 0, 0) == 0x10);
    CHECK(native_call(&cpu, 0xb0, 0x4e, 0, 65, 0x3000) == 1);
    CHECK(card_replaced_hook);
    card_replaced_hook();
    psx_native_card_vblank(&cpu);
    CHECK(card_flushes == 2 && test_card[0][65][0] == 0 &&
          read_word(EVENT_BASE + EVENT_SIZE + 4) == READY &&
          read_word(PSX_NATIVE_CARD_VOLUME_VALID) == 0);
    psx_native_card_bind(NULL);
    CHECK(!card_replaced_hook);
    CHECK(native_call(&cpu, 0xb0, 0x4c, 0, 0, 0) == 1);
    CHECK(read_word(PSX_NATIVE_CARD_STARTED) == 0 &&
          read_word(PSX_NATIVE_TIMER_AUTO_ACK + 12) == 1);
    native_call(&cpu, 0xa0, 0x30, 1, 0, 0);
    CHECK(native_call(&cpu, 0xa0, 0x2f, 0, 0, 0) == 16838);
    CHECK(native_call(&cpu, 0xa0, 0x2f, 0, 0, 0) == 5758);
    native_call(&cpu, 0xa0, 0x30, 1, 0, 0);
    CHECK(native_call(&cpu, 0xa0, 0x2f, 0, 0, 0) == 16838);
    native_call(&cpu, 0xa0, 0x39, 0x4000, 0x200, 0);
    uint32_t first = native_call(&cpu, 0xa0, 0x33, 24, 0, 0);
    CHECK(first == 0x4008 && read_word(first - 4) == 32);
    for (unsigned i = 0; i < 24; ++i) write_byte(first + i, (uint8_t)(i + 1));
    for (unsigned i = 0; i < 32; ++i) write_byte(0x4028 + i, 0xfe);
    uint32_t second = native_call(&cpu, 0xa0, 0x37, 2, 16, 0);
    CHECK(second == 0x4028);
    for (unsigned i = 0; i < 32; ++i) CHECK(!read_byte(second + i));
    uint32_t third = native_call(&cpu, 0xa0, 0x33, 16, 0, 0);
    CHECK(third == 0x4050);
    native_call(&cpu, 0xa0, 0x34, second, 0, 0);
    CHECK(native_call(&cpu, 0xa0, 0x38, first, 48, 0) == first);
    uint32_t moved = native_call(&cpu, 0xa0, 0x38, first, 80, 0);
    CHECK(moved && moved != first);
    for (unsigned i = 0; i < 24; ++i) CHECK(read_byte(moved + i) == i + 1);
    CHECK(native_call(&cpu, 0xa0, 0x38, moved, 24, 0) == moved);
    CHECK(native_call(&cpu, 0xa0, 0x37, UINT32_MAX, 2, 0) == 0);
    native_call(&cpu, 0xa0, 0x34, third, 0, 0);
    native_call(&cpu, 0xa0, 0x34, moved, 0, 0);
    CHECK(native_call(&cpu, 0xa0, 0x33, 0x1f0, 0, 0) == 0x4008);
    CHECK(native_call(&cpu, 0xa0, 0x33, 1, 0, 0) == 0);
    /* C0 kernel storage is independent of the user's exhausted A0 heap.
     * Restoring guest RAM alone must restore the allocator cursor/free list. */
    native_call(&cpu, 0xc0, 8, 0xa000e800u, 0x1000, 0);
    uint32_t kernel = native_call(&cpu, 0xb0, 0, 32, 0, 0);
    CHECK(kernel == 0xa000e808u);
    write_word(kernel, 0x12345678);
    uint32_t kernel_snapshot[sizeof ram / sizeof ram[0]];
    memcpy(kernel_snapshot, ram, sizeof ram);
    uint32_t event_storage = native_call(&cpu, 0xb0, 0, 32 * EVENT_SIZE, 0, 0);
    CHECK(event_storage && event_storage != kernel && read_word(kernel) == 0x12345678);
    memcpy(ram, kernel_snapshot, sizeof ram);
    CHECK(native_call(&cpu, 0xb0, 0, 32 * EVENT_SIZE, 0, 0) == event_storage);
    native_call(&cpu, 0xb0, 1, event_storage, 0, 0);
    CHECK(native_call(&cpu, 0xb0, 0, 32 * EVENT_SIZE, 0, 0) == event_storage);
    CHECK(native_call(&cpu, 0xa0, 0x33, 1, 0, 0) == 0);
    native_call(&cpu, 0xc0, 8, UINT32_MAX - 3, 32, 0);
    CHECK(native_call(&cpu, 0xb0, 0, 1, 0, 0) == 0);

    /* The real SDK reads these instruction immediates, then writes its
     * thread/event/stack configuration through the resulting guest pointer. */
    uint32_t info = read_word(0x474);
    CHECK(info == (0x80000000u | PSX_NATIVE_INFO_STUB));
    uint32_t configuration = (uint32_t)read_byte(info) | (uint32_t)read_byte(info + 1) << 8;
    configuration = (configuration << 16) + (int16_t)read_word(info + 4) - 8;
    CHECK(configuration == PSX_NATIVE_KERNEL_CONFIG);
    write_word(configuration, 12); write_word(configuration + 4, 32);
    write_word(configuration + 8, 0x801b6928);
    CHECK(read_word(PSX_NATIVE_KERNEL_CONFIG) == 12 &&
          read_word(PSX_NATIVE_KERNEL_CONFIG + 4) == 32);
    CHECK(native_call(&cpu, 0xa0, 0x9d, 0, 0, 0) == 0x20260101u);
    CHECK(native_call(&cpu, 0xa0, 0x9d, 2, 0, 0) == PSX_NATIVE_KERNEL_NAME);
    CHECK(read_byte(PSX_NATIVE_KERNEL_NAME) == 'P');
    cpu.gpr[4] = 7;
    CHECK(g_psx_bios_hle_hook(&cpu, PSX_NATIVE_INFO_STUB) && cpu.gpr[2] == 1024);
    CHECK(!psx_bios_hle_dispatch_quiet(PSX_NATIVE_INFO_STUB));
    CHECK(!g_psx_bios_hle_hook(&cpu, psx_native_bios_tables[0].stubs + 0x9d * 12));
    uint32_t info_word = read_word(info);
    write_word(info, 0);
    CHECK(!g_psx_bios_hle_hook(&cpu, PSX_NATIVE_INFO_STUB));
    write_word(info, info_word);
    test_gpu_status = 0x14000000u;
    CHECK(native_call(&cpu, 0xa0, 0x49, 0xe1001234u, 0, 0) == 0);
    CHECK(test_gpu_data == 0xe1001234u && test_gpu_writes == 1);
    native_call(&cpu, 0xa0, 0x48, 0x03000000u, 0, 0);
    CHECK(test_gpu_control == 0x03000000u);
    CHECK(native_call(&cpu, 0xa0, 0x4d, 0, 0, 0) == 0x14000000u);
    test_gpu_status = 0x54000000u;
    CHECK(native_call(&cpu, 0xa0, 0x4e, 0, 0, 0) == 0);
    CHECK(test_gpu_control == 0x04000000u);
    native_call(&cpu, 0xa0, 0x44, 0, 0, 0);
    CHECK(cache_resets == 1);
    CHECK(!g_psx_bios_hle_hook(&cpu, 0x80010000u & 0x1fffffffu));
}

static void check_native_kernel_registration(void)
{
    memset(ram, 0, sizeof ram);
    CPUState cpu = {0};
    cpu.read_word = read_word; cpu.write_word = write_word;
    cpu.read_byte = read_byte; cpu.write_byte = write_byte;
    psx_bios_hle_configure_native();
    psx_native_bios_init_tables(write_word);
    write_word(0x100, 0x1700);
    test_imask = 0x7ff;
    memset(test_timers, 0xff, sizeof test_timers);
    CHECK(native_call(&cpu, 0xc0, 0, 2, 0, 0) == 0);
    CHECK(read_word(0x1710) == PSX_NATIVE_IRQ_NODES + 3 * 12);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t node = PSX_NATIVE_IRQ_NODES + i * 12;
        CHECK(read_word(node) == (i ? node - 12 : 0));
        CHECK(read_word(node + 4) == (0x80000000u | (PSX_NATIVE_IRQ_TOKENS + i * 8)));
        CHECK(read_word(PSX_NATIVE_TIMER_AUTO_ACK + i * 4) == 1);
    }
    CHECK(test_imask == (0x7ffu & ~0x71u));
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned field = 0; field < 3; ++field) CHECK(test_timers[i][field] == 0);
    CHECK(native_call(&cpu, 0xc0, 1, 0, 0, 0) == 0);
    CHECK(read_word(0x1700) == PSX_NATIVE_IRQ_NODES + 5 * 12);
    write_word(PSX_NATIVE_GENERIC_IRQ_ACK, 1);
    CHECK(native_call(&cpu, 0xc0, 0x0c, 3, 0, 0) == 0);
    CHECK(read_word(0x1718) == PSX_NATIVE_IRQ_NODES + 4 * 12);
    CHECK(read_word(PSX_NATIVE_GENERIC_IRQ_ACK) == 0);
    uint32_t before[sizeof ram / sizeof ram[0]];
    memcpy(before, ram, sizeof ram);
    cpu.gpr[4] = 4;
    CHECK(psx_native_bios_service(&cpu, 0xc0, 0) == PSX_NATIVE_BIOS_UNSUPPORTED);
    CHECK(memcmp(before, ram, sizeof ram) == 0);
}

static void check_native_hardware_event_classes(void)
{
    memset(ram, 0, sizeof ram);
    memset(&irq_cpu, 0, sizeof irq_cpu);
    irq_cpu.read_word = read_word; irq_cpu.write_word = write_word;
    irq_cpu.read_byte = read_byte; irq_cpu.write_byte = write_byte;
    psx_bios_hle_configure_native();
    psx_native_bios_init_tables(write_word);
    write_word(0x100, 0x1700);
    psx_native_bios_boot_irq_handlers(write_word, 0x1700);
    write_word(0x120, EVENT_BASE); write_word(0x124, EVENT_SIZE);
    for (unsigned i = 0; i < 4; ++i) write_word(PSX_NATIVE_TIMER_AUTO_ACK + i * 4, 0);
    const uint8_t classes[] = {1, 2, 3, 4, 5, 6, 6, 8, 11, 9, 10};
    expected_saved_v0 = 0;
    irq_active = 1;
    for (unsigned irq = 0; irq < 11; ++irq) {
        add_event(0, 0x1000, MODE_MARK, 0);
        write_word(EVENT_BASE, 0xf0000000u + classes[irq]);
        write_word(PSX_NATIVE_GENERIC_IRQ_ACK + irq * 4, 1);
        irq_cpu.cop0[12] = 0x40000404u;
        irq_cpu.cop0[14] = CALLER_RESUME;
        test_istat = test_imask = 1u << irq;
        int escaped = setjmp(irq_escape);
        if (!escaped) {
            g_psx_bios_hle_hook(&irq_cpu, PSX_NATIVE_EXCEPTION_ENTRY);
            CHECK(0);
        }
        CHECK(escaped == 1 && !test_istat && read_word(EVENT_BASE + 4) == READY);
    }
    irq_active = 0;
}

static void check_native_root_counter_irq(void)
{
    memset(ram, 0, sizeof ram);
    memset(&irq_cpu, 0, sizeof irq_cpu);
    irq_cpu.read_word = read_word;
    irq_cpu.write_word = write_word;
    irq_cpu.read_byte = read_byte;
    irq_cpu.write_byte = write_byte;
    irq_cpu.cop0[12] = 0x40000404u;
    irq_cpu.cop0[14] = CALLER_RESUME;
    psx_bios_hle_configure_native();
    psx_native_bios_init_tables(write_word);

    write_word(0x100, 0x1700);
    psx_native_bios_boot_irq_handlers(write_word, 0x1700);
    const uint32_t timer_head = PSX_NATIVE_IRQ_NODES + 3 * 12;
    write_word(0x1744, IRQ_HANDLER);
    write_word(0x1748, IRQ_VERIFY_A);
    write_word(0x1754, IRQ_HANDLER);
    write_word(0x1758, IRQ_VERIFY_B);
    CHECK(native_call(&irq_cpu, 0xc0, 2, 1, 0x1740, 0) == 0);
    CHECK(native_call(&irq_cpu, 0xc0, 2, 1, 0x1750, 0) == 0);
    CHECK(read_word(0x1708) == 0x1750 && read_word(0x1750) == 0x1740);
    CHECK(native_call(&irq_cpu, 0xc0, 3, 1, 0x1740, 0) == 0x1740);
    CHECK(read_word(0x1750) == timer_head);
    CHECK(native_call(&irq_cpu, 0xc0, 2, 1, 0x1740, 0) == 0);
    CHECK(read_word(0x1708) == 0x1740 && read_word(0x1740) == 0x1750);
    call_count = 0;

    write_word(0x120, EVENT_BASE);
    write_word(0x124, 2 * EVENT_SIZE);
    add_event(0, 2, MODE_MARK, 0);
    write_word(EVENT_BASE, 0xf2000003u);
    add_event(1, 2, MODE_MARK, 0);
    write_word(EVENT_BASE + EVENT_SIZE, 0xf2000001u);
    write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 1);
    test_istat = test_imask = (1u << 0) | (1u << 5);

    irq_active = 1;
    int escaped = setjmp(irq_escape);
    if (!escaped) {
        g_psx_bios_hle_hook(&irq_cpu, PSX_NATIVE_EXCEPTION_ENTRY);
        CHECK(0);
    }
    CHECK(escaped == 1 && test_istat == (1u << 5));
    CHECK(call_count == 3 && calls[0] == IRQ_VERIFY_A &&
          calls[1] == IRQ_HANDLER && calls[2] == IRQ_VERIFY_B);
    CHECK(native_call(&irq_cpu, 0xc0, 3, 1, 0x1740, 0) == 0x1740);
    CHECK(native_call(&irq_cpu, 0xc0, 3, 1, 0x1750, 0) == 0x1750);
    CHECK(read_word(0x1708) == timer_head);
    CHECK(read_word(EVENT_BASE + 4) == READY &&
          read_word(EVENT_BASE + EVENT_SIZE + 4) == ENABLED);

    write_word(EVENT_BASE + 4, ENABLED);
    write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 0);
    irq_cpu.cop0[12] = 0x40000404u;
    test_istat = test_imask = 1;
    irq_cpu.gpr[29] = 0x801fe000;
    native_call(&irq_cpu, 0xa0, 0x13, 0x1900, 0, 0);
    native_call(&irq_cpu, 0xb0, 0x19, 0x1900, 0, 0);
    escaped = setjmp(irq_escape);
    if (!escaped) {
        g_psx_bios_hle_hook(&irq_cpu, PSX_NATIVE_EXCEPTION_ENTRY);
        CHECK(0);
    }
    CHECK(escaped == 2 && test_istat == 1);
    CHECK(read_word(EVENT_BASE + 4) == READY && irq_cpu.pc == CALLER_RESUME &&
          irq_cpu.gpr[2] == 1 && irq_cpu.gpr[29] == 0x801fe000);
    // With native BIOS CD ownership relinquished, the generic hardware IRQ
    // notification and its configured acknowledgement remain available.
    write_word(PSX_NATIVE_EXCEPTION_JUMP_PTR, 0);
    add_event(0, 0x1000, MODE_MARK, 0);
    write_word(EVENT_BASE, 0xf0000003u);
    write_word(PSX_NATIVE_GENERIC_IRQ_ACK + 2 * 4, 1);
    test_istat = test_imask = 1u << 2;
    irq_cpu.cop0[12] = 0x40000404u;
    escaped = setjmp(irq_escape);
    if (!escaped) {
        g_psx_bios_hle_hook(&irq_cpu, PSX_NATIVE_EXCEPTION_ENTRY);
        CHECK(0);
    }
    CHECK(escaped == 1 && test_istat == 0 && read_word(EVENT_BASE + 4) == READY);
    /* Simulate the writable SDK prefix's observed Cause load after it
     * saved the original V0. The continuation must preserve the real TCB
     * value when the existing scheduler snapshots the rest of the context. */
    write_word(PSX_NATIVE_EXCEPTION_ENTRY, 0);
    CHECK(!g_psx_bios_hle_hook(&irq_cpu, PSX_NATIVE_EXCEPTION_ENTRY));
    expected_saved_v0 = 0x12345678u;
    write_word(0x1010, expected_saved_v0);
    irq_cpu.gpr[2] = 0x400u;
    irq_cpu.cop0[12] = 0x40000404u;
    test_istat = test_imask = 0;
    escaped = setjmp(irq_escape);
    if (!escaped) {
        g_psx_bios_hle_hook(&irq_cpu, PSX_NATIVE_EXCEPTION_PATCH_RESUME);
        CHECK(0);
    }
    CHECK(escaped == 1);
    expected_saved_v0 = 0;
    irq_active = 0;
}

static void check_native_wait_event(void)
{
    CPUState cpu = {0};
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    psx_bios_hle_configure_native();
    psx_native_bios_init_tables(write_word);
    write_word(0x120, EVENT_BASE);
    write_word(0x124, EVENT_SIZE);
    write_word(EVENT_BASE + 4, 0x1000);
    wait_checks = 0;
    CHECK(native_call(&cpu, 0xb0, 0x0a, 0xf1000000, 0, 0) == 0);
    CHECK(!wait_checks && cpu.pc == CALLER_RESUME);
    write_word(EVENT_BASE + 4, READY);
    CHECK(native_call(&cpu, 0xb0, 0x0a, 0xf1000000, 0, 0) == 1);
    CHECK(read_word(EVENT_BASE + 4) == ENABLED && !wait_checks);
    wait_delivery = EVENT_BASE;
    CHECK(native_call(&cpu, 0xb0, 0x0a, 0xf1000000, 0, 0) == 1);
    CHECK(wait_checks == 1 && read_word(EVENT_BASE + 4) == ENABLED);
    CHECK(cpu.pc == CALLER_RESUME);
    /* An already waiting thread keeps waiting even if the event is disabled.
     * Its pointer and continuation are fully in the restored CPU/RAM state. */
    CPUState restored = cpu;
    restored.gpr[3] = EVENT_BASE;
    restored.pc = 0x80000000u | PSX_NATIVE_WAIT_EVENT_RESUME;
    write_word(EVENT_BASE + 4, 0x1000);
    CHECK(g_psx_bios_hle_hook(&restored, PSX_NATIVE_WAIT_EVENT_RESUME));
    CHECK(wait_checks == 2 && restored.gpr[2] == 1 && restored.pc == CALLER_RESUME);
    CHECK(read_word(EVENT_BASE + 4) == ENABLED);
    wait_delivery = 0;
}

static void check_native_files(void)
{
    memset(ram, 0, sizeof ram);
    memset(test_card, 0xff, sizeof test_card);
    test_card_present[0] = 1;
    test_card_present[1] = 0;
    test_card_flag[0] = 0;
    psx_bios_hle_configure_native();
    psx_native_bios_init_tables(write_word);
    CPUState cpu = {0};
    cpu.read_word = read_word; cpu.write_word = write_word;
    cpu.read_byte = read_byte; cpu.write_byte = write_byte; cpu.write_half = write_half;
    psx_native_card_bind(&cpu);
    native_call(&cpu, 0xb0, 0x4a, 0, 0, 0);
    native_call(&cpu, 0xb0, 0x4b, 0, 0, 0);
    const char path[] = "bu00:BASLUS-01395";
    for (unsigned i = 0; i < sizeof path; ++i) write_byte(0x1800 + i, (uint8_t)path[i]);
    CHECK(native_call(&cpu, 0xb0, 0x41, 0x1800, 0, 0) == 1);
    CHECK(!memcmp(test_card[0][0], test_card[0][63], 128));
    CHECK(read_word(0x140) == PSX_NATIVE_FILE_TABLE && read_word(0x144) == 16 * 44);
    CHECK(read_word(0x150) == PSX_NATIVE_DEVICE_TABLE && read_word(0x154) == 80);
    CHECK(native_call(&cpu, 0xb0, 0x32, 0x1800, 0x10200, 0) == 2);
    CHECK(test_card[0][1][0] == 0x51 && test_card[0][1][8] == 0xff);
    CHECK(native_call(&cpu, 0xb0, 0x36, 2, 0, 0) == 0);
    CHECK(native_call(&cpu, 0xa0, 0, 0x1800, 2, 0) == 2);
    for (unsigned i = 0; i < 8192; ++i) write_byte(0x4000 + i, (uint8_t)(i ^ 0x5a));
    unsigned flushes = card_flushes;
    CHECK(native_call(&cpu, 0xb0, 0x35, 2, 0x4000, 8192) == 8192);
    CHECK(card_flushes == flushes + 1); /* One persistent publication, not 64. */
    CHECK(test_card[0][64][0] == 0x5a && test_card[0][127][127] == (uint8_t)(8191 ^ 0x5a));
    CHECK(native_call(&cpu, 0xb0, 0x33, 2, 0, 0) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x34, 2, 0x6000, 8192) == 8192);
    for (unsigned i = 0; i < 8192; ++i) CHECK(read_byte(0x6000 + i) == (uint8_t)(i ^ 0x5a));
    CHECK(native_call(&cpu, 0xb0, 0x34, 2, 0x6000, 128) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x33, 2, 0, 0) == 0);
    card_flush_result = -1;
    CHECK(native_call(&cpu, 0xb0, 0x35, 2, 0x4000, 128) == UINT32_MAX);
    CHECK(read_word(PSX_NATIVE_FILE_TABLE + 2 * 44 + 16) == 0);
    card_flush_result = 0;
    CHECK(native_call(&cpu, 0xb0, 0x36, 2, 0, 0) == 0);
    CHECK(native_call(&cpu, 0xb0, 0x35, 2, 0x4000, 128) == UINT32_MAX);
    const char large_path[] = "bu00:TWO_BLOCKS";
    for (unsigned i = 0; i < sizeof large_path; ++i) write_byte(0x1a00 + i, (uint8_t)large_path[i]);
    CHECK(native_call(&cpu, 0xb0, 0x32, 0x1a00, 0x20200, 0) == 2);
    CHECK(test_card[0][2][0] == 0x51 && test_card[0][2][8] == 2 && test_card[0][3][0] == 0x53);
    for (unsigned i = 0; i < 16384; ++i) write_byte(0x4000 + i, (uint8_t)(i * 3));
    CHECK(native_call(&cpu, 0xb0, 0x35, 2, 0x4000, 16384) == 16384);
    CHECK(native_call(&cpu, 0xb0, 0x33, 2, 8192 - 64, 0) == 8192 - 64);
    CHECK(native_call(&cpu, 0xb0, 0x34, 2, 0x8000, 128) == 128);
    for (unsigned i = 0; i < 128; ++i) CHECK(read_byte(0x8000 + i) == (uint8_t)((8192 - 64 + i) * 3));
    CHECK(native_call(&cpu, 0xb0, 0x36, 2, 0, 0) == 0);
    /* The original X6 SDK wraps the writable bu firstFile callback. */
    const char pattern[] = "bu00:BASLUS*";
    for (unsigned i = 0; i < sizeof pattern; ++i) write_byte(0x1900 + i, (uint8_t)pattern[i]);
    write_word(0x120, EVENT_BASE); write_word(0x124, EVENT_SIZE * 2);
    add_event(0, 4, MODE_CALLBACK, CALLBACK_CARD_COMPLETE);
    add_event(1, 0x8000, MODE_CALLBACK, CALLBACK_CARD_COMPLETE);
    write_word(EVENT_BASE, 0xf0000011u);
    write_word(EVENT_BASE + EVENT_SIZE, 0xf0000011u);
    expected_card_device = 0; expected_card_spec = 4;
    card_file_completions = 0;
    write_word(PSX_NATIVE_DEVICE_TABLE + 0x34, CALLBACK_CARD_FIND);
    CHECK(native_call(&cpu, 0xb0, 0x42, 0x1900, 0x3000, 0) == 0x3000);
    CHECK(card_file_completions == 1 && cpu.gpr[31] == CALLER_RESUME);
    CHECK(card_find_overrides == 1 && read_word(0x3018) == 8192 && read_word(0x3020) == 64);
    for (unsigned i = 0; i < sizeof path - 5; ++i) CHECK(read_byte(0x3000 + i) == (uint8_t)path[5 + i]);
    CHECK(native_call(&cpu, 0xb0, 0x43, 0x3000, 0, 0) == 0);
    CHECK(card_file_completions == 1); /* NextFile does not start a header read. */
    const char missing[] = "bu00:NO_MATCH";
    for (unsigned i = 0; i < sizeof missing; ++i) write_byte(0x1940 + i, (uint8_t)missing[i]);
    CHECK(native_call(&cpu, 0xb0, 0x42, 0x1940, 0x3000, 0) == 0);
    CHECK(card_file_completions == 2); /* Completion is observable even with no match. */
    const char absent[] = "bu10:*";
    for (unsigned i = 0; i < sizeof absent; ++i) write_byte(0x1980 + i, (uint8_t)absent[i]);
    expected_card_device = 0x10; expected_card_spec = 0x8000;
    CHECK(native_call(&cpu, 0xb0, 0x42, 0x1980, 0x3000, 0) == 0);
    CHECK(card_file_completions == 3);
    /* Rejected requests do not manufacture a card completion. */
    write_word(PSX_NATIVE_CARD_STATUS, 2);
    CHECK(native_call(&cpu, 0xb0, 0x42, 0x1900, 0x3000, 0) == 0);
    write_word(PSX_NATIVE_CARD_STATUS, 1);
    CHECK(native_call(&cpu, 0xb0, 0x42, 0x1900, PSX_MAIN_RAM_WINDOW_BYTES, 0) == 0);
    CHECK(card_file_completions == 3);
    /* A readable, malformed volume still completed its raw header read. */
    expected_card_device = 0; expected_card_spec = 4;
    test_card[0][0][0] = 'X';
    CHECK(native_call(&cpu, 0xb0, 0x42, 0x1900, 0x3000, 0) == 0);
    CHECK(card_file_completions == 4);
    test_card[0][0][0] = 'M';
    CHECK(native_call(&cpu, 0xb0, 0x32, 0x1800, 1, 0) == 3);
    card_replaced_hook();
    CHECK(native_call(&cpu, 0xb0, 0x34, 3, 0x6000, 128) == UINT32_MAX);
    CHECK(read_word(PSX_NATIVE_CARD_FIND_FILE) == 0);
    /* _card_load completes its directory scan at the real card VBlank. */
    write_word(0x120, EVENT_BASE); write_word(0x124, EVENT_SIZE);
    add_event(0, 4, MODE_MARK, 0); write_word(EVENT_BASE, 0xf4000001u);
    CHECK(native_call(&cpu, 0xa0, 0xac, 0, 0, 0) == 1);
    psx_native_card_vblank(&cpu); psx_native_card_vblank(&cpu);
    CHECK(read_word(EVENT_BASE + 4) == READY && read_word(PSX_NATIVE_CARD_VOLUME_VALID) == 1);
    psx_native_card_bind(NULL);
}

int main(void)
{
    psx_bios_image.image_id = "OPENBIOS";
    psx_bios_image.image_sha256 = "unknown image";
    psx_bios_hle_configure(1, 0);
    CHECK(!psx_bios_hle_enabled() && !g_psx_bios_hle_hook);
    psx_bios_image.image_sha256 =
        "fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57";
    check_delivery(0);

    psx_bios_image.image_id = "SCPH-1001";
    psx_bios_image.image_sha256 = NULL;
    check_delivery(RETAIL_RESUME);
    check_native_calls();
    check_native_wait_event();
    check_native_kernel_registration();
    check_native_root_counter_irq();
    check_native_hardware_event_classes();
    check_native_files();
    puts("bios_hle_events_test: OK");
    return 0;
}
