#include "bios_hle.h"
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

static uint32_t ram[0x3000 / sizeof(uint32_t)];
static uint32_t calls[4];
static size_t call_count;
PsxBiosImageInfo psx_bios_image;
uint32_t g_psx_ram_size = sizeof ram;
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
    address &= 0x1fffffff;
    CHECK(!(address & 3) && address < sizeof ram);
    ram[address / 4] = value;
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
    puts("bios_hle_events_test: OK");
    return 0;
}
