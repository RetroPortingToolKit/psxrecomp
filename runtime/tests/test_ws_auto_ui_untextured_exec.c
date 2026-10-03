#ifdef NDEBUG
#undef NDEBUG
#endif

/* auto_ui_squash must correct untextured HUD fills together with the textured
 * frame around them. Spider-Man's health bar is a gouraud quad (GP0 0x38) and
 * its webbing meter a flat quad (0x28) inside textured frames (0x2C); while
 * only textured quads were admitted, the fills kept their raw 4:3 X and
 * overhung the squashed frames at 21:9 and 32:9.
 *
 * Drives the real path: an ordering-table linked list in guest RAM, the UI
 * prepass, run grouping (real ws_ui_group.c), then GP0 execution. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GPU_EXEC_REAL_UI_GROUP
#define GPU_EXEC_RECORD_SHADED_TEXTURED
#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

#define OT_HEAD   0x00010000u
#define NODE_RING 0x00010100u
#define NODE_FILL 0x00010200u
#define NODE_FLAT 0x00010300u
#define NODE_PANEL 0x00010400u
#define NODE_GAP   0x00010500u
#define NODE_REAR  0x00010600u   /* + 0x40 per rear piece */

static uint32_t pack_vertex(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void put_node(uint32_t addr, uint32_t next, const uint32_t *words,
                     uint32_t count) {
    test_ram[addr / 4u] = (count << 24) | (next & 0xFFFFFFu);
    for (uint32_t i = 0; i < count; i++)
        test_ram[addr / 4u + 1u + i] = words[i];
}

/* Frame: textured quad over [x0, x1). Fills inside it, both untextured. */
static void build_hud(int16_t x0, int16_t x1, int16_t fx0, int16_t fx1) {
    memset(test_ram, 0, sizeof(test_ram));
    const uint32_t frame[9] = {
        0x2C808080u, pack_vertex(x0, 20), 0x00000000u,
        pack_vertex(x1, 20), 0x00080000u,
        pack_vertex(x0, 36), 0x00001000u,
        pack_vertex(x1, 36), 0x00001010u,
    };
    const uint32_t gouraud[8] = {
        0x3800FF00u, pack_vertex(fx0, 26),
        0x0000FF80u, pack_vertex(fx1, 26),
        0x0000FF00u, pack_vertex(fx0, 30),
        0x0000FF80u, pack_vertex(fx1, 30),
    };
    const uint32_t flat[5] = {
        0x28802080u, pack_vertex(fx0, 31), pack_vertex(fx1, 31),
        pack_vertex(fx0, 34), pack_vertex(fx1, 34),
    };
    test_ram[OT_HEAD / 4u] = NODE_RING;              /* empty OT entry: rank 0 */
    put_node(NODE_RING, NODE_FILL, frame, 9);
    put_node(NODE_FILL, NODE_FLAT, gouraud, 8);
    put_node(NODE_FLAT, 0xFFFFFFu, flat, 5);
}

/* A flat backing panel one OT rank behind the HUD: OT_HEAD (empty, rank 0)
 * -> panel -> NODE_GAP (empty, rank 1) -> frame -> fills. */
static void add_backing_panel(int16_t x0, int16_t x1, int16_t y0, int16_t y1) {
    const uint32_t panel[5] = {
        0x28780000u, pack_vertex(x0, y0), pack_vertex(x1, y0),
        pack_vertex(x0, y1), pack_vertex(x1, y1),
    };
    test_ram[OT_HEAD / 4u] = NODE_PANEL;
    put_node(NODE_PANEL, NODE_GAP, panel, 5);
    test_ram[NODE_GAP / 4u] = NODE_RING;
}

/* Pieces one OT rank behind the HUD: OT_HEAD (empty, rank 0) -> pieces ->
 * NODE_GAP (empty, rank 1) -> frame -> fills. Each piece is a 4-vertex quad
 * (0x2C textured or 0x28 flat) over [x0,x1) x [y0,y1). */
typedef struct { uint32_t op; int16_t x0, x1, y0, y1; } RearPiece;
static uint32_t rear_node(int i) { return NODE_REAR + (uint32_t)i * 0x40u; }
static void add_rear_pieces(const RearPiece *pieces, int count) {
    for (int i = 0; i < count; i++) {
        const RearPiece *r = &pieces[i];
        uint32_t next = i + 1 < count ? rear_node(i + 1) : NODE_GAP;
        if (r->op == 0x2Cu) {
            const uint32_t q[9] = {
                0x2C808080u, pack_vertex(r->x0, r->y0), 0x00000000u,
                pack_vertex(r->x1, r->y0), 0x00080000u,
                pack_vertex(r->x0, r->y1), 0x00001000u,
                pack_vertex(r->x1, r->y1), 0x00001010u,
            };
            put_node(rear_node(i), next, q, 9);
        } else {
            const uint32_t q[5] = {
                0x28202020u, pack_vertex(r->x0, r->y0), pack_vertex(r->x1, r->y0),
                pack_vertex(r->x0, r->y1), pack_vertex(r->x1, r->y1),
            };
            put_node(rear_node(i), next, q, 5);
        }
    }
    test_ram[OT_HEAD / 4u] = rear_node(0);
    test_ram[NODE_GAP / 4u] = NODE_RING;
}

/* The same HUD list in the enhancement GPU-DMA aperture (THPS2's draw-distance
 * prim arena), with unrelated words in the main RAM its low bits alias. */
#define AP_BASE 0x00800000u
static void put_node_ap(uint32_t addr, uint32_t next, const uint32_t *words,
                        uint32_t count) {
    const uint32_t off = (addr - AP_BASE) / 4u;
    test_aperture[off] = (count << 24) | (next & 0xFFFFFFu);
    for (uint32_t i = 0; i < count; i++)
        test_aperture[off + 1u + i] = words[i];
}
static void move_hud_to_aperture(void) {
    const uint32_t nodes[3] = {NODE_RING, NODE_FILL, NODE_FLAT};
    test_aperture_used = sizeof test_aperture;
    memset(test_aperture, 0, sizeof test_aperture);
    for (int i = 0; i < 3; i++) {
        const uint32_t h = test_ram[nodes[i] / 4u], n = h >> 24;
        const uint32_t next = (h & 0xFFFFFFu) == 0xFFFFFFu
                                  ? 0xFFFFFFu : AP_BASE + (h & 0xFFFFFFu);
        put_node_ap(AP_BASE + nodes[i], next, &test_ram[nodes[i] / 4u + 1u], n);
    }
    test_ram[OT_HEAD / 4u] = AP_BASE + NODE_RING;
    for (uint32_t a = NODE_RING; a < NODE_FLAT + 0x40u; a += 4u)
        test_ram[a / 4u] = 0xDEAD0000u | a;
}

static void load_packet(uint32_t node, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        gp0_cmd_buf[i] = test_ram[node / 4u + 1u + i];
    gp0_cmd_source_addr = node + 4u;
    gp0_words_needed = (int)count;
}

static void reset_state(int in_place) {
    s_frame_count = 100;
    draw_offset_x = draw_offset_y = 0;
    draw_area_left = 0; draw_area_top = 0;
    draw_area_right = 1023; draw_area_bottom = 511;
    hres1 = 1; hres2 = 0; video_mode = 0; display_depth = 0;
    display_disabled = 0; display_area_x = 0; display_area_y = 0;
    h_display_x1 = 0x200; h_display_x2 = 0xC00;
    v_display_y1 = 0x010; v_display_y2 = 0x100;
    /* Projection-and-stretch path at 32:9: X squash 3/8, not native-wide. */
    ws_mode = 0;
    ws_cfg_num = 32; ws_cfg_den = 9;
    ws_xnum = 3; ws_xden = 8;
    ws_full_2d = 0;
    gpu_ws_set_auto_ui_squash(1);
    gpu_ws_set_auto_ui_in_place(in_place);
    gpu_ws_set_auto_ui_proportional(0);
}

/* Prepass the list and execute the two fills; returns their drawn X span. */
static void run_fills(int *gouraud_min, int *gouraud_max,
                      int *flat_min, int *flat_max) {
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);   /* frame + both untextured fills */

    gpu_exec_reset_triangles();
    load_packet(NODE_FILL, 8);
    gp0_exec_shaded_quad();
    assert(gpu_exec_triangles.calls == 2);
    *gouraud_min = gpu_exec_triangles.min_x;
    *gouraud_max = gpu_exec_triangles.max_x;

    gpu_exec_reset_triangles();
    load_packet(NODE_FLAT, 5);
    gp0_exec_mono_quad();
    assert(gpu_exec_triangles.calls >= 1);
    *flat_min = gpu_exec_triangles.min_x;
    *flat_max = gpu_exec_triangles.max_x;
}

/* Captured Ape intro GT4: world rank 3105, then a full-screen fade at 4095.
 * Relative ranks suffice here. The wall becomes exactly rectangular for a
 * few frames, but it must never become UI just because the fade is excluded. */
static void test_wall_behind_front_layer(int triangle) {
    reset_state(0);
    hres1 = 0; hres2 = 1; /* Ape's 384x240 display */
    ws_cfg_num = 16; ws_cfg_den = 9; ws_xnum = 3; ws_xden = 4;
    memset(test_ram, 0, sizeof(test_ram));
    const uint32_t wall[12] = {
        0x3C00FEFEu, 0x001C00C4u, 0x3ED8593Fu,
        0x0000FEFEu, 0x004100C4u, 0x0089653Fu,
        0x003FBEBEu, 0x001C008Bu, 0x006F591Fu,
        0x003FBEBEu, 0x0041008Bu, 0x0042651Fu,
    };
    const uint32_t fade[3] = {
        0x63000000u, pack_vertex(0, 0), 0x00F00180u,
    };
    const uint32_t front_triangle[4] = {
        0x20808080u, pack_vertex(1, 1), pack_vertex(8, 1), pack_vertex(1, 8),
    };
    test_ram[OT_HEAD / 4u] = NODE_RING;
    put_node(NODE_RING, NODE_GAP, wall, 12);
    test_ram[NODE_GAP / 4u] = NODE_FILL;
    put_node(NODE_FILL, NODE_FLAT, triangle ? front_triangle : fade,
             triangle ? 4 : 3);
    test_ram[NODE_FLAT / 4u] = 0xFFFFFFu; /* trailing empty rank */
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 0);
    gpu_exec_reset_triangles();
    load_packet(NODE_RING, 12);
    gp0_exec_shaded_textured_quad();
    assert(gpu_exec_triangles.calls == 2);
    assert(gpu_exec_triangles.min_x == 139);
    assert(gpu_exec_triangles.max_x == 196);
    assert(ws_auto_ui_transform_count == 0);
}

static void test_explicit_projection_widget(void) {
    const uint32_t tile = 0x20000u, glyph = 0x21000u;
    reset_state(0);
    hres1 = 0; hres2 = 1; /* Captured Ape display: 384x240. */
    h_display_x2 = h_display_x1 + 384u * 7u;
    ws_hud_sprt = 0;
    gpu_ws_set_auto_ui_squash(0); /* Explicit ownership is independent. */
    ws_hud_anchor_clear(ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE);
    memset(test_ram, 0, sizeof(test_ram));
    /* Actual Status backing tile shape, followed by a separate glyph list.
     * Replacing the prepass must not lose the earlier list's widget anchor. */
    const uint32_t backing[] = {0x76606060u, 0x00C60146u, 0x77ABB090u};
    const uint32_t text[] = {0x7D808080u, pack_vertex(54, 64), 0x7C5D9800u};
    put_node(tile, 0xFFFFFFu, backing, 3);
    put_node(glyph, 0xFFFFFFu, text, 3);
    gpu_ws_tag_hud_prim(tile, 0);
    gpu_ws_tag_hud_prim(glyph, 0);
    test_ram[OT_HEAD / 4u] = glyph;
    gpu_ws_prepass_linked_list(OT_HEAD);
    load_packet(tile, 3);
    last_scaled_rect.calls = 0;
    gp0_exec_textured_8x8();
    assert(last_scaled_rect.calls == 1);
    assert(last_scaled_rect.x == ws_scale_about(326, 192));
    assert(last_scaled_rect.w == 3);
    assert(last_scaled_rect.u1 - last_scaled_rect.u0 == 8);

    /* A stale legacy billboard classification cannot double-transform an
     * explicitly owned screen-space packet. */
    WsTag *legacy = &ws_tags[(glyph >> 2) & (WS_TAG_BUCKETS - 1)];
    legacy->key = glyph; legacy->stamp = (uint32_t)s_frame_count;
    legacy->anchor_x = 54;
    for (int edge = -1; edge <= 1; edge++) {
        gpu_ws_tag_hud_prim(glyph, edge);
        load_packet(glyph, 3);
        last_scaled_rect.calls = 0;
        gp0_exec_textured_16x16();
        assert(last_scaled_rect.calls == 1);
        assert(last_scaled_rect.x == ws_scale_about(54, (edge + 1) * 192));
        assert(last_scaled_rect.w == 6);
    }
    memset(ws_tags, 0, sizeof(ws_tags));

    /* Complete packet guards, frame expiry and native 4:3 identity. */
    load_packet(glyph, 3);
    gp0_cmd_buf[2] ^= 1u;
    last_textured_rect.calls = last_scaled_rect.calls = 0;
    gp0_exec_textured_16x16();
    assert(last_textured_rect.calls == 1 && last_textured_rect.x == 54);
    assert(last_scaled_rect.calls == 0);
    s_frame_count += 3;
    load_packet(glyph, 3);
    last_textured_rect.calls = last_scaled_rect.calls = 0;
    gp0_exec_textured_16x16();
    assert(last_textured_rect.calls == 1 && last_textured_rect.x == 54);
    assert(last_scaled_rect.calls == 0);
    gpu_ws_tag_hud_prim(glyph, 0);
    ws_xnum = ws_xden = 1;
    last_textured_rect.calls = last_scaled_rect.calls = 0;
    gp0_exec_textured_16x16();
    assert(last_textured_rect.calls == 1 && last_textured_rect.x == 54);
    assert(last_scaled_rect.calls == 0);

    /* Explicitly identified panels do not depend on automatic size/rank
     * heuristics; untagged world/fades still use the existing conservative gate. */
    ws_xnum = 3; ws_xden = 8;
    const uint32_t panel[] = {0x28202020u, pack_vertex(30, 30),
        pack_vertex(334, 30), pack_vertex(30, 206), pack_vertex(334, 206)};
    put_node(tile, 0xFFFFFFu, panel, 5);
    gpu_ws_tag_hud_prim(tile, 0);
    load_packet(tile, 5);
    gpu_exec_reset_triangles();
    gp0_exec_mono_quad();
    assert(gpu_exec_triangles.min_x == ws_scale_about(30, 192));
    assert(gpu_exec_triangles.max_x == ws_scale_about(334, 192));
    ws_hud_anchor_clear(ws_hud_anchor_tags, WS_HUD_ANCHOR_TABLE_SIZE);
}

int main(void) {
    int gmin, gmax, fmin, fmax;

    test_wall_behind_front_layer(0);
    test_wall_behind_front_layer(1);
    test_explicit_projection_widget();

    /* A real HUD remains eligible when its last drawing layer is followed
     * by an empty OT bucket, as in Ape's memory-card menu. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    test_ram[NODE_FLAT / 4u] = (5u << 24) | NODE_PANEL;
    test_ram[NODE_PANEL / 4u] = 0xFFFFFFu;
    run_fills(&gmin, &gmax, &fmin, &fmax);
    assert(gmin == ws_scale_about(64, 94));
    assert(gmax == ws_scale_about(120, 94));

    /* in_place: the fill and its frame share the run's own centre. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    run_fills(&gmin, &gmax, &fmin, &fmax);
    const int32_t centre = 60 + (128 - 60) / 2;
    assert(gmin == ws_scale_about(64, centre));
    assert(gmax == ws_scale_about(120, centre));
    assert(fmin == gmin && fmax == gmax);
    /* Inside the squashed frame, not overhanging it. */
    assert(gmin >= ws_scale_about(60, centre));
    assert(gmax <= ws_scale_about(128, centre));

    /* edges (default): a left-third run pins to the display's left edge. */
    reset_state(0);
    build_hud(60, 128, 64, 120);
    run_fills(&gmin, &gmax, &fmin, &fmax);
    const int32_t left = ws_disp_x();
    assert(gmin == ws_scale_about(64, left));
    assert(gmax == ws_scale_about(120, left));
    assert(fmin == gmin && fmax == gmax);

    /* auto_ui_size proportional at 32:9 (squash 3/8): the HUD shrinks by
     * sqrt((4/3) * 3/8) = sqrt(1/2) on both axes about its anchors. The run
     * spans y 20..36 in the top half, so it keeps its top edge (y 20). */
    reset_state(0);
    gpu_ws_set_auto_ui_proportional(1);
    build_hud(60, 128, 64, 120);
    run_fills(&gmin, &gmax, &fmin, &fmax);
    {
        const double s = sqrt(0.5);
        const int32_t left = ws_disp_x();
        assert(gmin == left + (int32_t)lround((64 - left) * 3.0 / 8.0 * s));
        assert(gmax == left + (int32_t)lround((120 - left) * 3.0 / 8.0 * s));
        gpu_exec_reset_triangles();
        load_packet(NODE_FILL, 8);
        gp0_exec_shaded_quad();
        assert(gpu_exec_triangles.min_y == 20 + (int32_t)lround(6 * s));
        assert(gpu_exec_triangles.max_y == 20 + (int32_t)lround(10 * s));
    }
    /* At 16:9 (squash 3/4) proportional is identical to original. */
    reset_state(0);
    ws_cfg_num = 16; ws_cfg_den = 9; ws_xnum = 3; ws_xden = 4;
    gpu_ws_set_auto_ui_proportional(1);
    build_hud(60, 128, 64, 120);
    run_fills(&gmin, &gmax, &fmin, &fmax);
    assert(gmin == ws_scale_about(64, ws_disp_x()));
    gpu_exec_reset_triangles();
    load_packet(NODE_FILL, 8);
    gp0_exec_shaded_quad();
    assert(gpu_exec_triangles.min_y == 26 && gpu_exec_triangles.max_y == 30);

    /* A non-axis-aligned untextured quad reaching outside every widget is
     * world geometry, never UI (one inside a widget is a part of it: see the
     * needle below). */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    test_ram[NODE_FILL / 4u + 1u + 3u] = pack_vertex(140, 25);  /* skew v1 out */
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 2);
    assert(ws_ui_reject.enclosed == 0);

    /* Backing panel enclosing the frame: admitted from the rank behind the
     * HUD and squashed with it about the shared run's centre. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    add_backing_panel(40, 300, 16, 40);
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 4);
    assert(ws_ui_reject.backing == 1);
    gpu_exec_reset_triangles();
    load_packet(NODE_PANEL, 5);
    gp0_exec_mono_quad();
    const int32_t run_centre = 40 + (300 - 40) / 2;
    assert(gpu_exec_triangles.min_x == ws_scale_about(40, run_centre));
    assert(gpu_exec_triangles.max_x == ws_scale_about(300, run_centre));

    /* The same quad not enclosing any HUD primitive is world geometry. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    add_backing_panel(200, 300, 16, 40);
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);
    assert(ws_ui_reject.backing == 0);

    /* Attached pieces (Spider-Man 2's webbing gauge): a textured segment
     * stacked one row under the frame, and a flat fill stacked under that
     * segment but not touching the frame, join through the fixed point. A
     * small piece with a gap wider than WS_UI_GROUP_STACK_GAP does not. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const RearPiece pieces[] = {
            {0x2Cu, 70, 100, 37, 52},   /* stacked on the frame (gap 1)   */
            {0x28u, 75,  90, 53, 60},   /* stacked on the segment only    */
            {0x2Cu, 70, 100, 70, 80},   /* 10 rows clear: stays out       */
        };
        add_rear_pieces(pieces, 3);
    }
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 5);
    assert(ws_ui_reject.backing == 2);
    gpu_exec_reset_triangles();
    load_packet(rear_node(1), 5);
    gp0_exec_mono_quad();
    /* The fill squashes about the whole widget's centre, not its own. */
    const int32_t gauge_centre = 60 + (128 - 60) / 2;
    assert(gpu_exec_triangles.min_x == ws_scale_about(75, gauge_centre));
    assert(gpu_exec_triangles.max_x == ws_scale_about(90, gauge_centre));

    /* A large textured quad overlapping the HUD is world geometry. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const RearPiece pieces[] = {{0x2Cu, 0, 200, 0, 120}};
        add_rear_pieces(pieces, 1);
    }
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);
    assert(ws_ui_reject.backing == 0);

    /* A letterbox bar spanning the whole display width in the HUD's own
     * rank is a full-width overlay: never UI, drawn edge to edge. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const int16_t bx0 = (int16_t)ws_disp_x(), bx1 = (int16_t)(ws_disp_x() + ws_disp_w());
        const uint32_t bar[5] = {
            0x28000000u, pack_vertex(bx0, 0), pack_vertex(bx1, 0),
            pack_vertex(bx0, 30), pack_vertex(bx1, 30),
        };
        put_node(NODE_FLAT, 0xFFFFFFu, bar, 5);
        gpu_ws_prepass_linked_list(OT_HEAD);
        assert(ws_ui_prepass_count == 2);        /* frame + gouraud fill only */
        assert(ws_ui_reject.too_big == 1);
        /* A rule stopping two pixels short of each edge is full width too. */
        const uint32_t rule[5] = {
            0x28000000u, pack_vertex((int16_t)(bx0 + 2), 40), pack_vertex((int16_t)(bx1 - 2), 40),
            pack_vertex((int16_t)(bx0 + 2), 44), pack_vertex((int16_t)(bx1 - 2), 44),
        };
        put_node(NODE_FLAT, 0xFFFFFFu, rule, 5);
        gpu_ws_prepass_linked_list(OT_HEAD);
        assert(ws_ui_prepass_count == 2 && ws_ui_reject.too_big == 1);
        put_node(NODE_FLAT, 0xFFFFFFu, bar, 5);
        gpu_exec_reset_triangles();
        load_packet(NODE_FLAT, 5);
        gp0_exec_mono_quad();
        assert(gpu_exec_triangles.min_x == bx0);
        assert(gpu_exec_triangles.max_x == bx1);
    }

    /* A needle inside its widget (Spider-Man's compass arrow: flat
     * triangles in the ring's rank) joins the widget and squashes with it. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const uint32_t tri[4] = {
            0x20FFFF00u, pack_vertex(70, 22), pack_vertex(100, 30), pack_vertex(80, 34),
        };
        put_node(NODE_FLAT, 0xFFFFFFu, tri, 4);
        gpu_ws_prepass_linked_list(OT_HEAD);
        assert(ws_ui_prepass_count == 3);        /* frame, fill, needle */
        assert(ws_ui_reject.enclosed == 1);
        gpu_exec_reset_triangles();
        load_packet(NODE_FLAT, 4);
        gp0_exec_mono_tri();
        const int32_t widget = 60 + (128 - 60) / 2;
        assert(gpu_exec_triangles.min_x == ws_scale_about(70, widget));
        assert(gpu_exec_triangles.max_x == ws_scale_about(100, widget));
    }

    /* A triangle reaching outside every widget is world geometry. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const uint32_t tri[4] = {
            0x20FFFF00u, pack_vertex(50, 22), pack_vertex(140, 30), pack_vertex(80, 34),
        };
        put_node(NODE_FLAT, 0xFFFFFFu, tri, 4);
        gpu_ws_prepass_linked_list(OT_HEAD);
        assert(ws_ui_prepass_count == 2);
        assert(ws_ui_reject.enclosed == 0);
        gpu_exec_reset_triangles();
        load_packet(NODE_FLAT, 4);
        gp0_exec_mono_tri();
        assert(gpu_exec_triangles.min_x == 50 && gpu_exec_triangles.max_x == 140);
    }

    /* A list in the aperture validates against its own words at DMA time:
     * the prepass survives the walk and the fill squashes as in RAM. A RAM
     * node at the aliased offset is a different node. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    move_hud_to_aperture();
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);
    {
        const uint32_t nodes[3] = {NODE_RING, NODE_FILL, NODE_FLAT};
        const uint32_t stale = ws_ui_reject.stale;
        for (int i = 0; i < 3; i++) {
            const uint32_t a = AP_BASE + nodes[i];
            const uint32_t h = test_aperture[(a - AP_BASE) / 4u];
            gpu_ws_validate_linked_list_header(a, h);
            gpu_ws_validate_linked_list_node(a, h >> 24);
        }
        assert(ws_ui_reject.stale == stale && ws_ui_prepass_count == 3);
        gpu_exec_reset_triangles();
        for (uint32_t i = 0; i < 8; i++)
            gp0_cmd_buf[i] = test_aperture[(NODE_FILL + 4u) / 4u + i];
        gp0_cmd_source_addr = AP_BASE + NODE_FILL + 4u;
        gp0_words_needed = 8;
        gp0_exec_shaded_quad();
        const int32_t centre = 60 + (128 - 60) / 2;
        assert(gpu_exec_triangles.min_x == ws_scale_about(64, centre));
        assert(gpu_exec_triangles.max_x == ws_scale_about(120, centre));
        assert(GPU_RAM_KEY(AP_BASE + NODE_FILL) != GPU_RAM_KEY(NODE_FILL));
        gpu_ws_validate_linked_list_header(NODE_FILL, test_ram[NODE_FILL / 4u]);
        assert(ws_ui_reject.stale == stale + 1 && ws_ui_stale_why[2] > 0);
    }
    test_aperture_used = 0;

    puts("ws_auto_ui_untextured_exec_test: PASS");
    return 0;
}
