#ifdef NDEBUG
#undef NDEBUG
#endif

/* Reveal-edge quads through the real GP0 handlers: tagged flat and one-texel
 * textured quads add bands only outside the authored quad, with the quad's
 * blend, CLUT, texture page and texel, while the original draw, guest RAM and
 * every untagged or invalid case stay exactly as before. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/gpu.c"

/* Record the renderer calls this test observes instead of the shared stubs. */
#define gr_draw_flat_rect reveal_stub_flat_rect
#define gr_draw_textured_rect_scaled reveal_stub_scaled_rect
#define gr_set_semi_transparency reveal_stub_semi
#define gr_set_color_modulation reveal_stub_modulation
#include "gpu_exec_stubs.inc"
#undef gr_draw_flat_rect
#undef gr_draw_textured_rect_scaled
#undef gr_set_semi_transparency
#undef gr_set_color_modulation

typedef struct { int x, y, w, h, u0, v0, u1, v1, clut_x, clut_y, tpage, semi, mode; uint16_t color; } Rect;
static Rect flat_rects[8], scaled_rects[8];
static int flat_count, scaled_count, cur_semi, cur_mode, cur_r, cur_g, cur_b;

void gr_set_semi_transparency(int enabled, int mode) { cur_semi = enabled; cur_mode = mode; }
void gr_set_color_modulation(int r, int g, int b, int raw_texture) {
    (void)raw_texture; cur_r = r; cur_g = g; cur_b = b;
}
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    assert(flat_count < 8);
    flat_rects[flat_count++] = (Rect){x, y, w, h, 0, 0, 0, 0, 0, 0, 0, cur_semi, cur_mode, color};
}
void gr_draw_textured_rect_scaled(int x, int y, int w, int h, int u0, int v0,
                                  int u1, int v1, uint16_t clut_x,
                                  uint16_t clut_y, uint16_t texpage) {
    assert(scaled_count < 8);
    scaled_rects[scaled_count++] = (Rect){x, y, w, h, u0, v0, u1, v1, clut_x,
                                          clut_y, texpage, cur_semi, cur_mode, 0};
}

static uint32_t pack_vertex(int x, int y) {
    return (uint16_t)(int16_t)x | ((uint32_t)(uint16_t)(int16_t)y << 16);
}

/* Display and draw registers of a 320-wide frame buffer at VRAM (0,0). */
static void apply_test_registers(void) {
    gp0_cmd_source_addr = 0xFFFFFFFFu;
    draw_offset_x = 0;
    draw_offset_y = 0;
    draw_area_left = 0;
    draw_area_top = 0;
    draw_area_right = 1023;
    draw_area_bottom = 511;
    hres1 = 1;
    hres2 = 0;
    video_mode = 0;
    display_depth = 0;
    display_disabled = 0;
    display_area_x = 0;
    display_area_y = 0;
    h_display_x1 = 0x200;
    h_display_x2 = 0xC00;
    v_display_y1 = 0x010;
    v_display_y2 = 0x100;
    semi_transparency = 2;
    ws_mode = 0;
    ws_cfg_num = 4;
    ws_cfg_den = 3;
    ws_xnum = 1;
    ws_xden = 1;
    ws_full_2d = 0;
}

static void reset_gpu_state_for_test(void) {
    memset(test_ram, 0, sizeof(test_ram));
    gpu_reset_state(0);
    s_frame_count = 100;
    apply_test_registers();
}

static void configure_native_wide_16_9(void) {
    ws_mode = 2;
    ws_cfg_num = 16;
    ws_cfg_den = 9;
    ws_full_2d = 1;
}

/* Place a packet at command address `addr` and execute it. */
static void exec_packet(uint32_t addr, const uint32_t *words, uint32_t n) {
    memcpy(&test_ram[addr / 4u], words, n * sizeof(uint32_t));
    memcpy(gp0_cmd_buf, words, n * sizeof(uint32_t));
    gp0_cmd_source_addr = addr;
    gp0_words_needed = (int)n;
    flat_count = scaled_count = 0;
    gpu_exec_reset_triangles();
    if (((words[0] >> 24) & 0xFCu) == 0x28u) gp0_exec_mono_quad();
    else gp0_exec_textured_quad();
}

#define TINT_ADDR 0x00010004u
#define BAR_ADDR  0x00010104u
#define LINE_ADDR 0x00010204u

static void tint_packet(uint32_t w[5], int x0, int x1) {
    w[0] = 0x2A604020u;
    w[1] = pack_vertex(x0, 40); w[2] = pack_vertex(x1, 40);
    w[3] = pack_vertex(x0, 200); w[4] = pack_vertex(x1, 200);
}

/* One-texel bar: texel (16, texel_v), CLUT (512,480), texture page 5. */
static void bar_packet(uint32_t w[9], int x0, int x1, int y0, int y1, int texel_v) {
    const uint32_t uv = 16u | ((uint32_t)texel_v << 8);
    const uint32_t clut = (512u / 16u) | (480u << 6);
    w[0] = 0x2C808080u;
    w[1] = pack_vertex(x0, y0); w[2] = uv | (clut << 16);
    w[3] = pack_vertex(x1, y0); w[4] = uv | (5u << 16);
    w[5] = pack_vertex(x0, y1); w[6] = uv;
    w[7] = pack_vertex(x1, y1); w[8] = uv;
}

static void test_translucent_tint(int W, int M) {
    uint32_t w[5];
    tint_packet(w, -24, W / 2);
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    exec_packet(TINT_ADDR, w, 5);                      /* untagged reference */
    assert(flat_count == 1);
    const Rect original = flat_rects[0];
    assert(original.x == -24 && original.w == W / 2 + 24 && original.y == 40 && original.h == 160);

    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    exec_packet(TINT_ADDR, w, 5);
    assert(flat_count == 2);
    /* Band first, then the untouched original; same colour, same blend. */
    assert(flat_rects[0].x == -M && flat_rects[0].w == M - 24);
    assert(flat_rects[0].y == 40 && flat_rects[0].h == 160);
    assert(flat_rects[0].color == original.color);
    assert(flat_rects[0].semi == 1 && flat_rects[0].mode == 2);
    assert(memcmp(&flat_rects[1], &original, sizeof original) == 0);
    /* The band ends where the quad begins: no overlap, no double blend. */
    assert(flat_rects[0].x + flat_rects[0].w == original.x);

    uint32_t ram[5];
    memcpy(ram, &test_ram[TINT_ADDR / 4u], sizeof ram);
    assert(memcmp(ram, w, sizeof ram) == 0);           /* guest packet unchanged */
}

static void test_one_texel_bar(int W, int M) {
    uint32_t w[9];
    bar_packet(w, -20, W + 20, -20, 42, 24);
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    draw_offset_y = 8;
    exec_packet(BAR_ADDR, w, 9);
    assert(scaled_count == 1);                          /* original only */
    const Rect original = scaled_rects[0];

    gpu_ws_tag_reveal_edge_quad(0x80000000u | (BAR_ADDR - 4u), WS_REVEAL_EDGE_BOTH);
    exec_packet(BAR_ADDR, w, 9);
    assert(scaled_count == 3);
    for (int i = 0; i < 2; i++) {
        const Rect *b = &scaled_rects[i];
        assert(b->u0 == 16 && b->u1 == 16 && b->v0 == 24 && b->v1 == 24);
        assert(b->clut_x == 512 && b->clut_y == 480 && b->tpage == 5);
        assert(b->y == -20 + 8 && b->h == 62);
    }
    assert(scaled_rects[0].x == -M && scaled_rects[0].w == M - 20);
    assert(scaled_rects[1].x == W + 20 && scaled_rects[1].w == M - 20);
    assert(memcmp(&scaled_rects[2], &original, sizeof original) == 0);
    assert(cur_r == 0x80 && cur_g == 0x80 && cur_b == 0x80);

    /* A line whose inner end joins other artwork grows only at the edge. */
    uint32_t l[9];
    bar_packet(l, 40, W + 20, 20, 21, 28);
    memcpy(&test_ram[LINE_ADDR / 4u], l, sizeof l);
    gpu_ws_tag_reveal_edge_quad(LINE_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    exec_packet(LINE_ADDR, l, 9);
    assert(scaled_count == 2);
    assert(scaled_rects[0].x == W + 20 && scaled_rects[0].w == M - 20);
    assert(scaled_rects[1].x == 40);
}

static void test_rejections(int W) {
    uint32_t w[9], t[5];
    bar_packet(w, -20, W + 20, -20, 42, 24);
    tint_packet(t, -24, W / 2);

    /* Native 4:3: nothing is tagged, nothing grows. */
    reset_gpu_state_for_test();
    memcpy(&test_ram[BAR_ADDR / 4u], w, sizeof w);
    gpu_ws_tag_reveal_edge_quad(BAR_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    assert(!ws_reveal_edge_tags_used);
    exec_packet(BAR_ADDR, w, 9);
    assert(scaled_count == 1);

    /* Two texels, bad alignment, no side, wrong primitive. */
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    w[8] ^= 1u;
    memcpy(&test_ram[BAR_ADDR / 4u], w, sizeof w);
    gpu_ws_tag_reveal_edge_quad(BAR_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    w[8] ^= 1u;
    memcpy(&test_ram[TINT_ADDR / 4u], t, sizeof t);
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 2u, WS_REVEAL_EDGE_BOTH);
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, 0);
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, 4);
    const uint32_t tri[4] = {0x20604020u, pack_vertex(-24, 40), pack_vertex(9, 40), pack_vertex(-24, 96)};
    memcpy(&test_ram[LINE_ADDR / 4u], tri, sizeof tri);
    gpu_ws_tag_reveal_edge_quad(LINE_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    assert(!ws_reveal_edge_tags_used);

    /* Tagged, then the packet changes: the guard drops the tag. */
    memcpy(&test_ram[TINT_ADDR / 4u], t, sizeof t);
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    t[1] = pack_vertex(-25, 40); t[3] = pack_vertex(-25, 200);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);

    /* Expiry after the shared freshness window. */
    tint_packet(t, -24, W / 2);
    memcpy(&test_ram[TINT_ADDR / 4u], t, sizeof t);
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    s_frame_count += WS_HUD_ANCHOR_FRESH_FRAMES;
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 2);
    s_frame_count += 1;
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);

    /* Native-wide switched off after tagging. */
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    ws_mode = 0;
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);
    configure_native_wide_16_9();

    /* An explicit HUD anchor on the same packet wins. */
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    gpu_ws_tag_hud_prim(TINT_ADDR - 4u, 1);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);
}

static void test_reset_and_savestate(int W) {
    uint32_t t[5];
    tint_packet(t, -24, W / 2);
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    memcpy(&test_ram[TINT_ADDR / 4u], t, sizeof t);

    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    gpu_reset_state(0);
    apply_test_registers();
    configure_native_wide_16_9();
    assert(!ws_reveal_edge_tags_used);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);

    static uint8_t snap[1u << 22];
    const uint32_t n = gpu_snapshot_bytes();
    assert(n <= sizeof snap);
    gpu_snapshot_write(snap);
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 2);
    assert(gpu_snapshot_read(snap, n));
    configure_native_wide_16_9();
    assert(!ws_reveal_edge_tags_used);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);
}

/* Render-pass checkpoints carry the reveal-edge table with the rest of the
 * packet provenance: a restore brings back the tags present at the
 * checkpoint, drops tags added after it, and keeps explicit HUD tags. */
static int hud_anchor_of(uint32_t addr, const uint32_t *words, uint32_t n) {
    int anchor = 0;
    if (!ws_hud_anchor_lookup(ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE,
                              GPU_RAM_KEY(addr), words, n,
                              (uint32_t)s_frame_count, &anchor))
        return 99;
    return anchor;
}

static void test_render_pass_checkpoint(int W) {
    uint32_t t[5], b[9], h[5];
    tint_packet(t, -24, W / 2);
    bar_packet(b, W / 2, W + 20, 30, 31, 24);
    tint_packet(h, W - 40, W - 8);   /* an ordinary right-hand HUD panel */
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    memcpy(&test_ram[TINT_ADDR / 4u], t, sizeof t);
    memcpy(&test_ram[BAR_ADDR / 4u], b, sizeof b);
    memcpy(&test_ram[LINE_ADDR / 4u], h, sizeof h);

    /* Checkpoint holding one reveal tag and one HUD tag. */
    gpu_ws_tag_reveal_edge_quad(TINT_ADDR - 4u, WS_REVEAL_EDGE_BOTH);
    gpu_ws_tag_hud_prim(LINE_ADDR - 4u, 1);
    assert(hud_anchor_of(LINE_ADDR, h, 5) == 1);
    assert(gpu_pass_checkpoint_save());

    /* The pass loses the tint tag and gains a bar tag. */
    ws_hud_anchor_clear(ws_reveal_edge_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    gpu_ws_tag_reveal_edge_quad(BAR_ADDR - 4u, WS_REVEAL_EDGE_RIGHT);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 1);                    /* quad only */
    exec_packet(BAR_ADDR, b, 9);
    assert(scaled_count == 2);                  /* right band, plus bar */

    gpu_pass_checkpoint_restore();
    configure_native_wide_16_9();
    assert(ws_reveal_edge_tags_used);
    exec_packet(TINT_ADDR, t, 5);
    assert(flat_count == 2);                    /* band restored, plus quad */
    const int M = (int)draw_area_wide_x_margin();
    assert(flat_rects[0].x == -M && flat_rects[0].w == M - 24);
    exec_packet(BAR_ADDR, b, 9);
    assert(scaled_count == 1);                  /* later tag dropped: bar only */
    assert(hud_anchor_of(LINE_ADDR, h, 5) == 1);

    /* A checkpoint taken before any reveal tag restores the unused state. */
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    memcpy(&test_ram[BAR_ADDR / 4u], b, sizeof b);
    assert(gpu_pass_checkpoint_save());
    gpu_ws_tag_reveal_edge_quad(BAR_ADDR - 4u, WS_REVEAL_EDGE_RIGHT);
    assert(ws_reveal_edge_tags_used);
    gpu_pass_checkpoint_restore();
    configure_native_wide_16_9();
    assert(!ws_reveal_edge_tags_used);
    exec_packet(BAR_ADDR, b, 9);
    assert(scaled_count == 1);                  /* bar only */
}

/* Auto-UI grouping (squash mode) and reveal edges (native-wide) never run in
 * the same configuration: native-wide leaves the X squash at 1/1, so the
 * prepass admits nothing and a tagged frame piece cannot move a HUD group;
 * in squash mode the tag is inert. */
#define LIST_PANEL 0x00011000u
#define LIST_BAR   0x00011100u
static void put_list_node(uint32_t addr, uint32_t next, const uint32_t *w,
                          uint32_t n) {
    test_ram[addr / 4u] = (n << 24) | (next & 0xFFFFFFu);
    memcpy(&test_ram[addr / 4u + 1u], w, n * sizeof(uint32_t));
}

static void test_auto_ui_prepass_exclusive(int W) {
    uint32_t h[5], b[9];
    tint_packet(h, W - 40, W - 8);
    bar_packet(b, W / 2, W + 20, 30, 31, 24);
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    gpu_ws_set_auto_ui_squash(1);
    put_list_node(LIST_PANEL, LIST_BAR, h, 5);
    put_list_node(LIST_BAR, 0xFFFFFFu, b, 9);
    gpu_ws_tag_reveal_edge_quad(LIST_BAR, WS_REVEAL_EDGE_RIGHT);

    gpu_ws_prepass_linked_list(LIST_PANEL);
    assert(!ws_active() && ws_ui_prepass_count == 0);
    exec_packet(LIST_BAR + 4u, b, 9);
    assert(scaled_count == 2);                  /* band, plus bar */

    /* Squash mode: the reveal tag does nothing. */
    gpu_ws_configure(16, 9, 0, 0, 1);
    assert(!ws_native_wide_active());
    exec_packet(LIST_BAR + 4u, b, 9);
    assert(scaled_count == 1);                  /* bar only, no band */
    gpu_ws_set_auto_ui_squash(0);
}

int main(void) {
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    assert(ws_native_wide_active());
    const int W = (int)ws_disp_w(), M = (int)draw_area_wide_x_margin();
    assert(W > 0 && M > 24);
    test_translucent_tint(W, M);
    test_one_texel_bar(W, M);
    test_rejections(W);
    test_reset_and_savestate(W);
    test_render_pass_checkpoint(W);
    test_auto_ui_prepass_exclusive(W);
    printf("ws_reveal_edge_exec_test: PASS (display %d, reveal %d)\n", W, M);
    return 0;
}
