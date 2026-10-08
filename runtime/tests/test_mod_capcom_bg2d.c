#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mod_capcom_bg2d.h"

static unsigned char memory[10u * 1024u * 1024u];
static uint32_t writes;
uint8_t psx_mod_read_byte(uint32_t a) { return memory[a & 0x1fffffffu]; }
uint16_t psx_mod_read_half(uint32_t a) {
    a &= 0x1fffffffu; return memory[a] | (uint16_t)memory[a+1] << 8;
}
uint32_t psx_mod_read_word(uint32_t a) {
    a &= 0x1fffffffu;
    return memory[a] | (uint32_t)memory[a+1] << 8 | (uint32_t)memory[a+2] << 16 | (uint32_t)memory[a+3] << 24;
}
void psx_mod_write_word(uint32_t a, uint32_t w) {
    a &= 0x1fffffffu; assert(a <= sizeof memory - 4); ++writes;
    for (unsigned i=0; i<4; ++i) memory[a+i] = (uint8_t)(w >> (8*i));
}
uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t n, uint32_t align) {
    assert(n == 6u * 1024u * 1024u && align == 32u); return 0x400000u;
}
void gpu_ws_bg2d_set_host_arena(uint32_t base, uint32_t bytes) {
    assert(base == 0x400000u && bytes == 6u * 1024u * 1024u);
}

int main(void) {
    WsViewAnchor centered = {106,106,0,0,0};
    WsViewAnchor mapped = psx_capcom_scroll_view(centered,800,667);
    assert(mapped.left==133 && mapped.right==79 && mapped.shift==27);
    WsViewAnchor edge = {212,0,106,0,0};
    mapped = psx_capcom_scroll_view(edge,800,535);
    assert(mapped.left==265 && mapped.right==0 && mapped.shift==159);
    /* Native fractional scroll crosses integer boundaries without replacing
     * its quantization with a floating-point scale of the view shift. */
    mapped=psx_capcom_parallax_view((WsViewAnchor){53,53,0,0,0},2,65);
    assert(mapped.left==26 && mapped.right==80 && mapped.shift==-27);
    PSXCapcomTileMap map = {0x80010000u,0x80011000u,0x80014000u,4,1,4,0,0,3};
    /* Native map cell0 points to metatile1. A patterned row proves the
     * wide output reads authored tile coordinates rather than a32-col ring. */
    for (unsigned i=0; i<4; ++i) memory[0x10000+i]=1;
    for (unsigned i=0; i<256; ++i) {
        uint16_t tile=(i&1) ? 0xc002u : 1u;
        memory[0x11200+i*2]=(uint8_t)tile;
        memory[0x11200+i*2+1]=(uint8_t)(tile>>8);
    }
    psx_mod_write_word(0x80014004u,0x01123400u);
    psx_mod_write_word(0x80014008u,0x42123400u);
    assert(psx_capcom_map_tile(&map,0,0)==1);
    assert(psx_capcom_map_tile(&map,528,0)==0xc002u);
    assert(psx_capcom_map_tile(&map,-16,0)==0);
    assert(psx_capcom_map_tile(&map,1024,0)==0);
    assert(psx_capcom_map_tile(&map,0,256)==0);
    map.width=5;
    assert(psx_capcom_map_tile(&map,0,0)==0); /* Invalid dimensions. */
    map.width=4;
    /* SLUS01334 original80028144..80028164 palette recipe: base7900,
     * descriptor bitsF000 shifted6, bits0F00 shifted8, page40 shifted4. */
    assert(psx_capcom_tile_uvclut(0x42123400u,0x7900u)==0x7dc41020u);
    assert(psx_capcom_tile_uvclut(0x42123400u,0x7980u)==0x7e441020u);
    uint32_t arena=psx_capcom_background_activate(3);
    PSXCapcomBackground b={0x80015000u,0x80016000u,3,17,0x7900};
    psx_mod_write_word(b.heads+408,0xdeadbeefu); /* Other display buffer. */
    psx_mod_write_word(b.heads+68,0x12345678u); /* Other layer. */
    psx_mod_write_word(0x80017004u,0x7d808080u);
    WsViewAnchor v={0,224,112,0,0};
    assert(psx_capcom_background_render(&b,arena,0,&map,0,0,v,0x80017000u,NULL)==560);
    assert(psx_mod_read_word(b.heads+408)==0xdeadbeefu);
    assert(psx_mod_read_word(b.heads+68)==0x12345678u);
    assert(psx_mod_read_word(arena+8)==0);
    assert(psx_mod_read_word(arena+32+4)==0x7f808080u); /* Tile semi-transparency. */
    assert(psx_mod_read_word(arena+32+12)==0x7dc41020u);
    assert(psx_mod_read_word(arena+16)==112);
    assert(psx_mod_read_word(arena+28)==GPU_WS_BG2D_PACKET_MAGIC);
    assert((psx_mod_read_word(b.heads+3u*68u+2u*4u)&0xffffffu)==arena+32);
    /* Column33 atx528 exists past the512px native ring. */
    assert((int16_t)psx_mod_read_word(arena+33u*32u+8u)==528);
    assert(psx_mod_read_word(arena+560u*32u)==0); /* Slice overrun sentinel. */
    writes=0;
    v.left=32768;
    assert(psx_capcom_background_render(&b,arena,0,&map,15,0,v,0x80017000u,NULL)==-1);
    assert(writes==0); /* Invalid setup must not erase native lists. */
    v=(WsViewAnchor){0,0,0,0,0};
    assert(psx_capcom_background_render(&b,arena,0,&map,0,0,v,0x80017000u,NULL)==0);
    assert(writes==0); /* Native4:3 identity. */
    puts("Capcom adaptive map/palette/OT/buffer bounds passed");
    return 0;
}
