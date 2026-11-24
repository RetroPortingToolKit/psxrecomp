#include "bios_hle.h"
#include "native_bios_services.h"
#include "cpu_state.h"
#include "psx_bios_image.h"
#include "psx_cycles.h"
#include "psx_memory.h"

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
    CALLER_RESUME = 0x1000c,
    NESTED_RESUME = 0x10010,
    RETAIL_RESUME = 0x1bfc,
    ENABLED = 0x2000,
    READY = 0x4000,
    MODE_CALLBACK = 0x1000,
    MODE_MARK = 0x2000,
};

static uint32_t ram[0x4000 / sizeof(uint32_t)];
static uint32_t calls[4];
static size_t call_count;
PsxBiosImageInfo psx_bios_image;
uint32_t g_psx_ram_size = sizeof ram;
uint32_t g_psx_ram_mask = sizeof ram - 1;
static unsigned cache_resets;
void psx_icache_reset(void) { ++cache_resets; }
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle = UINT64_MAX;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
int g_exec_phase, g_event_step_conservative, g_ls_replay_active, psx_in_device_service;

#define CHECK(test) do { \
    if (!(test)) { fprintf(stderr, "B0 event fixture line %d: %s\n", __LINE__, #test); exit(1); } \
} while (0)

int fntrace_is_game_started(void) { return 1; }
int psx_get_in_exception(void) { return 0; }
void psx_restore_state_escape(void) { CHECK(0); }
void psx_scheduler_resume_at(uint32_t pc) { (void)pc; CHECK(0); }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_devices_service_to_now(void) { CHECK(0); }
void psx_advance_cycles_slow(uint32_t cycles) { (void)cycles; CHECK(0); }

static uint32_t read_word(uint32_t address)
{
    address &= 0x1fffffff;
    CHECK(!(address & 3) && address < sizeof ram);
    return ram[address / 4];
}

static void write_word(uint32_t address, uint32_t value)
{
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
    CHECK(strcmp(psx_bios_hle_backend_name(), "HLE (native kernel)") == 0);
    CPUState cpu = {0};
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    cpu.read_byte = read_byte;
    cpu.write_byte = write_byte;

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
    CHECK(native_call(&cpu, 0xb0, 0x0f, 0xff000001, 0, 0) == 1);
    CHECK(native_call(&cpu, 0xc0, 0x05, 0, 0, 0) == 1);

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
    native_call(&cpu, 0xa0, 0x44, 0, 0, 0);
    CHECK(cache_resets == 1);
    CHECK(!g_psx_bios_hle_hook(&cpu, 0x80010000u & 0x1fffffffu));
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
    puts("bios_hle_events_test: OK");
    return 0;
}
