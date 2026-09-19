#include "mod_plugins.h"
#include "mod_memory.h"
#include "mod_texture_banks.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { uint16_t* pixels; uint32_t width, height; } Bank;
typedef struct { uint32_t begin, size; } PacketArena;
static Bank banks[65536];
static PacketArena arenas[16];
static unsigned arena_count;
static uint32_t bank_bytes;
static PSXModTextureBankResolver resolve_bank;
static int resolving;
static int bank_batching;

void psx_mod_set_texture_bank_batching(int enabled) { bank_batching = enabled != 0; }
int mod_texture_bank_batchable(int immutable, int mask_check, int semi) {
    return bank_batching && immutable && !mask_check &&
        (semi == 0 || semi == 1 || semi == 3);
}

void psx_mod_set_texture_bank_resolver(PSXModTextureBankResolver resolver) {
    resolve_bank = resolver;
}

int psx_mod_define_texture_bank(uint16_t id, uint32_t w, uint32_t h,
                                const uint16_t* pixels) {
    uint32_t bytes;
    Bank* bank = &banks[id];
    if (!id || !pixels || !w || !h || w > 1024u || h > 512u) return 0;
    bytes = w * h * 2u;
    if (bank->pixels)
        return bank->width == w && bank->height == h &&
               memcmp(bank->pixels, pixels, bytes) == 0;
    if (bytes > 256u * 1024u * 1024u - bank_bytes) return 0;
    bank->pixels = (uint16_t*)malloc(bytes);
    if (!bank->pixels) return 0;
    memcpy(bank->pixels, pixels, bytes);
    bank->width = w; bank->height = h;
    bank_bytes += bytes;
    return 1;
}

const uint16_t* mod_texture_bank_pixels(uint16_t id, uint32_t* w, uint32_t* h) {
    if (!id || !w || !h) return NULL;
    if (!banks[id].pixels && resolve_bank && !resolving) {
        resolving = 1;
        (void)resolve_bank(id);
        resolving = 0;
    }
    *w = banks[id].width; *h = banks[id].height;
    return banks[id].pixels;
}

uint32_t psx_mod_alloc_texture_packet_memory(uint32_t size, uint32_t alignment) {
    uint32_t addr;
    if (arena_count == 16u || size < 40u) return 0;
    addr = psx_mod_gpu_dma_memory_alloc(size, alignment);
    if (!addr) return 0;
    arenas[arena_count].begin = addr & 0x1FFFFFFFu;
    arenas[arena_count++].size = size;
    return addr;
}

uint16_t mod_texture_packet_bank(uint32_t source, const uint32_t* words, uint32_t count) {
    unsigned i;
    uint32_t address = source & 0x1FFFFFFFu;
    if (!words || count != 9u || (words[0] >> 26) != 0xDu) return 0;
    for (i = 0; i < arena_count; ++i) {
        if (address >= arenas[i].begin && address - arenas[i].begin <= arenas[i].size - 36u)
            return (uint16_t)((words[3] >> 24) | ((words[6] >> 24) << 8));
    }
    return 0;
}

int mod_texture_packet_precision(uint32_t source, float q[3], float xy[6]) {
    unsigned i, j;
    uint32_t address = source & 0x1FFFFFFFu;
    for (i = 0; i < arena_count; ++i) {
        if (arenas[i].size < 80u || address < arenas[i].begin ||
            address - arenas[i].begin > arenas[i].size - 76u) continue;
        if (psx_mod_read_word(source + 36u) != 0x48545031u) return 0;
        for (j = 0; j < 9u; ++j) {
            uint32_t bits = psx_mod_read_word(source + 40u + j * 4u);
            float f;
            memcpy(&f, &bits, sizeof f);
            if (!isfinite(f)) return 0;
            if (j < 3u) {
                if (f <= 0.0f || f > 1.0f) return 0;
                q[j] = f;
            } else {
                if (f < -2048.0f || f > 2048.0f) return 0;
                xy[j - 3u] = f;
            }
        }
        return 1;
    }
    return 0;
}
