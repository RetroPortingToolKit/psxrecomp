/* The native BIOS sources are compiled into every runtime target, including
 * the REFERENCE oracle and PSX_NATIVE_KERNEL=OFF builds. Link them exactly as
 * those targets do (no PSX_NATIVE_KERNEL) and check that the card provider's
 * event publisher still delivers through the guest's own EvCB table. */
#include "cpu_state.h"
#include "native_bios_services.h"
#include "psx_bios_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PSX_NATIVE_KERNEL) && PSX_NATIVE_KERNEL
#error "this fixture covers builds without the native kernel"
#endif

enum {
    EVENT_BASE = 0x2000, EVENT_SIZE = 0x1c,
    ENABLED = 0x2000, READY = 0x4000, MODE_CALLBACK = 0x1000, MODE_MARK = 0x2000,
    CARD_CLASS = 0xf4000001u, SUCCESS = 4, CALLBACK = 0x10000,
};

#define CHECK(test) do { \
    if (!(test)) { fprintf(stderr, "reference BIOS fixture line %d: %s\n", __LINE__, #test); exit(1); } \
} while (0)

static uint32_t ram[0x20000 / sizeof(uint32_t)];
static unsigned callbacks;
PsxBiosImageInfo psx_bios_image;
uint32_t g_psx_ram_size = sizeof ram, g_psx_ram_mask = sizeof ram - 1;
uint64_t psx_cycle_count, psx_next_service_cycle = UINT64_MAX;
uint32_t g_psx_cyc_batch, g_psx_cyc_batch_limit, g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
int g_exec_phase, g_event_step_conservative, g_ls_replay_active, psx_in_device_service;

static void unexpected(const char *boundary)
{
    fprintf(stderr, "reference BIOS fixture reached %s\n", boundary);
    exit(1);
}

int cdrom_has_disc(void) { unexpected("cdrom_has_disc"); return 0; }
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }
void psx_devices_service_to_now(void) { unexpected("device service"); }
uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
int psx_get_in_exception(void) { return 0; }
void psx_icache_reset(void) { unexpected("icache reset"); }
int psx_mod_read_disc_file(const char *path, void *buffer, uint32_t capacity, uint32_t *size)
{
    (void)path; (void)buffer; (void)capacity; (void)size;
    unexpected("disc read");
    return 0;
}
uint32_t psx_sched_current_tcb(CPUState *cpu) { (void)cpu; unexpected("scheduler"); return 0; }
int psx_syscall(CPUState *cpu, uint32_t code) { (void)cpu; (void)code; unexpected("syscall"); return 0; }
uint8_t sio_card_flag(int port) { (void)port; unexpected("card flag"); return 0; }
void sio_card_mark_written(int port) { (void)port; unexpected("card write"); }
uint32_t sio_read_pad_packet(int port, uint8_t out[34]) { (void)port; (void)out; unexpected("pad"); return 0; }
void sio_set_cards_replaced_hook(void (*hook)(void)) { (void)hook; }
int fntrace_is_game_started(void) { return 1; }
int memcard_is_present(int port) { (void)port; unexpected("memcard"); return 0; }
int memcard_read_sector(int port, int sector, uint8_t *buffer)
{
    (void)port; (void)sector; (void)buffer;
    unexpected("memcard read");
    return -1;
}
int memcard_write_sector(int port, int sector, const uint8_t *buffer)
{
    (void)port; (void)sector; (void)buffer;
    unexpected("memcard write");
    return -1;
}
void memcard_flush(int port) { (void)port; unexpected("memcard flush"); }
int memcard_last_flush_result(int port) { (void)port; unexpected("memcard flush"); return -1; }

void psx_dispatch_call(CPUState *cpu, uint32_t function, uint32_t return_address)
{
    CHECK(function == CALLBACK && return_address == PSX_NATIVE_IRQ_CALLBACK_LINK);
    CHECK(cpu->gpr[31] == PSX_NATIVE_IRQ_CALLBACK_LINK);
    ++callbacks;
}

static uint32_t read_word(uint32_t address)
{
    address &= 0x1fffffffu;
    CHECK(!(address & 3u) && address < sizeof ram);
    return ram[address / 4];
}

static void write_word(uint32_t address, uint32_t value)
{
    address &= 0x1fffffffu;
    CHECK(!(address & 3u) && address < sizeof ram);
    ram[address / 4] = value;
}

static void put_event(unsigned index, uint32_t mode, uint32_t function)
{
    const uint32_t event = EVENT_BASE + index * EVENT_SIZE;
    write_word(event, CARD_CLASS);
    write_word(event + 4, ENABLED);
    write_word(event + 8, SUCCESS);
    write_word(event + 12, mode);
    write_word(event + 16, function);
}

int main(void)
{
    CPUState cpu = {0};
    cpu.read_word = read_word;
    cpu.write_word = write_word;
    write_word(0x120, EVENT_BASE);
    write_word(0x124, 2 * EVENT_SIZE);
    put_event(0, MODE_MARK, 0);
    put_event(1, MODE_CALLBACK, CALLBACK);
    cpu.gpr[31] = 0x80012340u;

    psx_native_bios_event(&cpu, CARD_CLASS, SUCCESS, 1);
    CHECK(read_word(EVENT_BASE + 4) == READY);
    CHECK(callbacks == 1 && cpu.gpr[31] == 0x80012340u);
    psx_native_bios_event(&cpu, CARD_CLASS, SUCCESS, 0);
    CHECK(read_word(EVENT_BASE + 4) == ENABLED);
    CHECK(callbacks == 1);

    puts("bios_hle_reference_test: OK");
    return 0;
}
