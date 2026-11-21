/* PSX-SPX kernelbios: "Garbage Area at Address 00000000h" and
 * "C(07h) - InstallExceptionHandlers()". These bytes are guest RAM, including
 * when a card-writing routine uses address zero as its source buffer.
 * Link actual memory.c and fntrace.c; only unrelated device hooks are doubles. */
#include "harness_common.h"
#include "fntrace.h"
#include "psx_bios_known_images.h"
#include <stdio.h>
#include <string.h>

void memory_init(const char*);
uint8_t* memory_get_ram_ptr(void);
void psx_write_word(uint32_t, uint32_t);
uint32_t psx_read_word(uint32_t);
extern PsxBiosImageInfo psx_bios_image;
extern uint32_t g_debug_last_store_pc;

static int failures;
#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", message); ++failures; } } while (0)

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    CPUState cpu = {0};
    const uint32_t stub[4] = {0x3c1a0000u, 0x275a0c80u, 0x03400008u, 0u};
    /* The authored fixture ROM contains a valid store at the historical key.
     * Active-image metadata is supplied as it is by the generated BIOS host;
     * this is a memory-map test, not a retail BIOS boot qualification. */
    psx_bios_image.image_crc32 = psx_known_bios_by_stem("SCPH1001")->crc32;
    memory_init(argv[1]);
    fntrace_set_game_range(0x80010000u, 0u);
    for (unsigned i = 0; i < 4; ++i) {
        psx_write_word(0x80000000u + 4u*i, stub[i]);
        psx_write_word(0x80000080u + 4u*i, stub[i]);
    }
    g_debug_last_store_pc = 0xbfc04e90u;
    psx_write_word(0x80000000u, 3u); /* boot-time CD delay's documented word */
    uint8_t expected[16];
    memcpy(expected, memory_get_ram_ptr(), sizeof expected);
    fntrace_mark_game_started(&cpu);
    CHECK(!memcmp(memory_get_ram_ptr(), expected, sizeof expected),
          "game-entry handoff preserves guest low-RAM bytes");
    CHECK(psx_read_word(0x80000084u) == stub[1], "real exception vector remains intact");

    /* The post-entry relocated installation is an ordinary guest store too.
     * Make its opcode match the authored ROM so the old suppression really
     * triggers; a mismatched image/word would make the negative meaningless. */
    g_debug_last_store_pc = 0x80010000u;
    psx_write_word(0x00000f00u, 0xaf5a0000u);
    psx_write_word(0x00000004u, 0u);
    g_debug_last_store_pc = 0x00000f00u;
    psx_write_word(0xa0000004u, stub[1]);
    CHECK(psx_read_word(0x80000004u) == stub[1],
          "relocated installation reaches actual low RAM after game entry");

    /* Keep the separately scoped post-entry delay-loop policy unchanged. */
    g_debug_last_store_pc = 0x80010000u;
    psx_write_word(0x00000000u, 0x11111111u);
    g_debug_last_store_pc = 0xbfc04e90u;
    psx_write_word(0x80000000u, 0u);
    CHECK(psx_read_word(0x00000000u) == 0x11111111u,
          "independent delay-loop suppression remains unchanged");
    if (failures) return 1;
    puts("production game-entry/low-RAM store contract: PASS");
    return 0;
}
