#ifndef PSX_MOD_CAPCOM_BG2D_H
#define PSX_MOD_CAPCOM_BG2D_H

#include "mod_plugins.h"
#include "gpu.h"
#include "ws_view_anchor.h"
#include <stdint.h>

/* Capcom's 16px tile engine: resolve the authored map, never a wrapping
 * guest tile ring. The title supplies addresses, palette layout and policies.
 * Host packets use the modern GPU view metadata and snapshot-owned DMA RAM.
 * No guest camera, producer loop, ring, or native packet budget is changed. */
typedef struct PSXCapcomTileMap {
    uint32_t map, metatiles, descriptors;
    unsigned width, height, layer_stride, layer, left_screen, right_screen;
} PSXCapcomTileMap;

typedef struct PSXCapcomBackground {
    uint32_t heads, tails;
    unsigned layers, buckets, clut_base;
} PSXCapcomBackground;

#define PSX_CAPCOM_BG_LAYER_BYTES (1024u * 1024u)

static inline int psx_capcom_ram_range(uint32_t p, uint32_t n) {
    p &= 0x1fffffffu;
    return p >= 0x10000u && p < 0x200000u && n <= 0x200000u - p;
}

static inline uint16_t psx_capcom_map_tile(const PSXCapcomTileMap *m, int x, int y) {
    if (x < 0 || y < 0 || !m->width || !m->height ||
        !m->layer_stride || m->width > m->layer_stride / m->height) return 0;
    unsigned col = (unsigned)x / 256u, row = (unsigned)y / 256u;
    if (col < m->left_screen || col > m->right_screen || col >= m->width || row >= m->height)
        return 0;
    uint64_t cell = (uint64_t)m->map + m->layer * (uint64_t)m->layer_stride + row * m->width + col;
    if (cell > UINT32_MAX || !psx_capcom_ram_range((uint32_t)cell, 1)) return 0;
    uint64_t tile = (uint64_t)m->metatiles + psx_mod_read_byte((uint32_t)cell) * 512u +
        (((unsigned)y / 16u) & 15u) * 32u + (((unsigned)x / 16u) & 15u) * 2u;
    return tile <= UINT32_MAX && psx_capcom_ram_range((uint32_t)tile, 2)
         ? psx_mod_read_half((uint32_t)tile) : 0;
}

static inline uint32_t psx_capcom_tile_uvclut(uint32_t desc, unsigned clut_base) {
    unsigned page = desc >> 24;
    unsigned clut = clut_base + ((desc & 0xf000u) >> 6) + ((page & 0x40u) << 4);
    clut |= (desc >> 8) & 15u;
    return (clut << 16) | ((desc >> 12) & 0xf0u) | (((desc >> 16) & 0xf0u) << 8);
}

static inline int psx_capcom_floor_div(int n, int d) {
    return n >= 0 ? n / d : -((-n + d - 1) / d);
}

/* Use the same fractional scroll mapping as the retail parallax function.
 * The title chooses the divisor; independent atlas layers may use their own
 * authored bounds instead. */
static inline WsViewAnchor psx_capcom_parallax_view(WsViewAnchor fg, int divisor, int camera) {
    int extra = (fg.left + fg.right + fg.pad_left + fg.pad_right) / 2;
    int origin = psx_capcom_floor_div(camera, divisor) -
        psx_capcom_floor_div(camera - fg.left, divisor) + fg.pad_left;
    WsViewAnchor result = {origin, 2 * extra - origin, origin - extra, fg.pad_left, fg.pad_right};
    return result;
}

/* Optional per-tile title policy (panorama wrapping, retained texture bank,
 * or authored region substitution). NULL uses live VRAM and the native map. */
typedef void (*PSXCapcomTilePolicy)(unsigned layer, int *x, int *y,
                                   unsigned *mirror_x, uint16_t *bank);

static inline uint32_t psx_capcom_background_activate(unsigned layers) {
    if (!layers || layers > 8u) return 0;
    uint32_t bytes = 2u * layers * PSX_CAPCOM_BG_LAYER_BYTES;
    uint32_t arena = psx_mod_alloc_gpu_dma_memory(bytes, 32u);
    if (arena) gpu_ws_bg2d_set_host_arena(arena, bytes);
    return arena;
}

/* Return emitted packet count, or -1 if setup is invalid. All setup checks
 * precede replacing the current layer's OT lists. Both its priority groups
 * and the other display buffer stay distinct. Only the native callback that
 * follows this operation splices these groups into the final game OT. */
static inline int psx_capcom_background_render(const PSXCapcomBackground *b,
        uint32_t arena, unsigned buffer, const PSXCapcomTileMap *map,
        int sx, int sy, WsViewAnchor view, uint32_t native_packet,
        PSXCapcomTilePolicy policy) {
    if (!arena || !b->layers || b->layers > 8u || map->layer >= b->layers || buffer > 1u ||
        !b->buckets || b->buckets > 64u || !map->width || !map->height ||
        !psx_capcom_ram_range(map->map, 1) || !psx_capcom_ram_range(map->metatiles, 512) ||
        !psx_capcom_ram_range(map->descriptors, 4)) return -1;
    if (view.left <= 0 && view.right <= 0) return 0;
    if (view.left < 0 || view.right < 0 || view.left > 32768 || view.right > 32768) return -1;
    int left = (view.left + 15) / 16, right = (view.right + 15) / 16;
    int columns = left + 21 + right;
    int screen_x = -(sx & 15), screen_y = -(sy & 15);
    if (columns > 2048 || screen_x - left * 16 < -32768 ||
        screen_x + (20 + right) * 16 > 32767) return -1;
    unsigned group_bytes = b->buckets * 4u, buffer_bytes = 2u * b->layers * group_bytes;
    uint32_t ot_bytes = 2u * buffer_bytes;
    if (!psx_capcom_ram_range(b->heads, ot_bytes) || !psx_capcom_ram_range(b->tails, ot_bytes)) return -1;
    uint32_t cursor = arena + (buffer * b->layers + map->layer) * PSX_CAPCOM_BG_LAYER_BYTES;
    uint32_t first = cursor;
    uint32_t color = psx_mod_read_word(native_packet + 4u);
    if (((color >> 24) & 0xfdu) != 0x7du) color = 0x7d808080u;
    int start_col = sx / 16, start_row = sy / 16; /* Retail truncates negative scroll. */
    for (unsigned group = map->layer; group < 2u * b->layers; group += b->layers) {
        for (unsigned i = 0; i < b->buckets; ++i) {
            uint32_t offset = buffer * buffer_bytes + group * group_bytes + i * 4u;
            psx_mod_write_word(b->heads + offset, 0);
            psx_mod_write_word(b->tails + offset, b->heads + offset);
        }
    }
    for (int row = 0; row < 16; ++row) for (int col = -left; col < 21 + right; ++col) {
        int x = (start_col + col) * 16, y = (start_row + row) * 16;
        unsigned mirror = 0;
        uint16_t bank = 0;
        if (policy) policy(map->layer, &x, &y, &mirror, &bank);
        uint16_t tile = psx_capcom_map_tile(map, x, y);
        if (!tile) continue;
        uint64_t desc_addr = (uint64_t)map->descriptors + (tile & 0x3fffu) * 4u;
        if (desc_addr > UINT32_MAX || !psx_capcom_ram_range((uint32_t)desc_addr, 4)) continue;
        uint32_t desc = psx_mod_read_word((uint32_t)desc_addr);
        unsigned bucket = (desc >> 24) & 0x3fu;
        if ((desc >> 24) == 255u || bucket >= b->buckets) continue;
        unsigned group = map->layer + ((tile & 0x8000u) ? b->layers : 0u);
        uint32_t tail_slot = b->tails + buffer * buffer_bytes + group * group_bytes + bucket * 4u;
        uint32_t tail = psx_mod_read_word(tail_slot);
        psx_mod_write_word(cursor, 0x03000000u);
        psx_mod_write_word(cursor + 4u, (color & ~0x02000000u) | ((tile & 0x4000u) ? 0x02000000u : 0u));
        psx_mod_write_word(cursor + 8u, (uint16_t)(screen_x + col * 16) |
            ((uint32_t)(uint16_t)(screen_y + row * 16) << 16));
        psx_mod_write_word(cursor + 12u, psx_capcom_tile_uvclut(desc, b->clut_base));
        psx_mod_write_word(cursor + 16u, bank ? (uint16_t)view.shift | (uint32_t)bank << 16 : (uint32_t)view.shift);
        psx_mod_write_word(cursor + 20u, (uint32_t)view.pad_left);
        psx_mod_write_word(cursor + 24u, (uint32_t)view.pad_right);
        psx_mod_write_word(cursor + 28u, (bank ? GPU_WS_BG2D_BANK_PACKET_MAGIC : GPU_WS_BG2D_PACKET_MAGIC) |
            (mirror ? GPU_WS_BG2D_MIRROR_X : 0u));
        psx_mod_write_word(tail, (psx_mod_read_word(tail) & 0xff000000u) | (cursor & 0xffffffu));
        psx_mod_write_word(tail_slot, cursor);
        cursor += 32u;
    }
    return (int)((cursor - first) / 32u);
}
#endif
