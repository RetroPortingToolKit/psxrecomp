#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/gpu.c"

uint64_t s_frame_count;
int g_exec_phase;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
CPUState *debug_cpu_ptr;
int g_psx_vram_dirty_tracking;

static uint32_t test_ram[0x00200000u / 4u];

static struct {
    int calls;
    int x, y, w, h;
    int u, v;
    uint16_t clut_x, clut_y, texpage;
} last_textured_rect;

static uint32_t pack_vertex(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void reset_gpu_state_for_test(void) {
    memset(test_ram, 0, sizeof(test_ram));
    memset(&last_textured_rect, 0, sizeof(last_textured_rect));
    memset(ws_hud_anchor_tags, 0, sizeof(ws_hud_anchor_tags));
    memset(ws_tags, 0, sizeof(ws_tags));

    s_frame_count = 100;
    gp0_cmd_source_addr = 0xFFFFFFFFu;
    gp0_words_needed = 3;
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
    ws_mode = 0;
    ws_cfg_num = 4;
    ws_cfg_den = 3;
    ws_xnum = 1;
    ws_xden = 1;
    ws_anchor_addr = 0;
    ws_full_2d = 0;
    ws_nw_hud_corners = 0;
    ws_nw_hud_tag_rects = 0;
    ws_nw_left_hud_packet_lo = 0;
    ws_nw_left_hud_packet_hi = 0;
}

static void set_dot_packet(uint32_t command_addr, int16_t x, int16_t y) {
    gp0_cmd_buf[0] = 0x6C204060u;
    gp0_cmd_buf[1] = pack_vertex(x, y);
    gp0_cmd_buf[2] = 0x01230A0Bu;
    gp0_cmd_source_addr = command_addr;
    if (command_addr < sizeof(test_ram))
        memcpy(&test_ram[command_addr / 4u], gp0_cmd_buf, 3u * sizeof(uint32_t));
}

static void exec_dot_and_expect(int expected_x, int expected_y) {
    last_textured_rect.calls = 0;
    gp0_exec_textured_dot();
    assert(last_textured_rect.calls == 1);
    assert(last_textured_rect.x == expected_x);
    assert(last_textured_rect.y == expected_y);
    assert(last_textured_rect.w == 1);
    assert(last_textured_rect.h == 1);
    assert(last_textured_rect.u == 0x0B);
    assert(last_textured_rect.v == 0x0A);
}

static void configure_native_wide_16_9(void) {
    ws_mode = 2;
    ws_cfg_num = 16;
    ws_cfg_den = 9;
    ws_xnum = 1;
    ws_xden = 1;
    ws_full_2d = 1;
}

int main(void) {
    reset_gpu_state_for_test();
    set_dot_packet(0x10004u, 137, 42);
    draw_offset_x = 11;
    draw_offset_y = -7;
    exec_dot_and_expect(148, 35);

    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    set_dot_packet(0x10004u, 137, 42);
    gpu_ws_tag_hud_prim(0x10000u, -1);
    draw_offset_x = 11;
    draw_offset_y = -7;
    exec_dot_and_expect(95, 35);

    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    ws_nw_hud_corners = 1;
    set_dot_packet(0x10004u, 105, 42);
    draw_offset_x = 2;
    exec_dot_and_expect(54, 42);

    puts("gpu_textured_dot_nw_shift_exec_test: PASS");
    return 0;
}

uint32_t psx_read_word(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys <= 0x00200000u - 4u)
        return test_ram[phys / 4u];
    return 0;
}

uint16_t psx_read_half(uint32_t addr) {
    uint32_t word = psx_read_word(addr & ~3u);
    return (uint16_t)(word >> ((addr & 2u) ? 16 : 0));
}

uint8_t psx_read_byte(uint32_t addr) {
    uint32_t word = psx_read_word(addr & ~3u);
    return (uint8_t)(word >> ((addr & 3u) * 8u));
}

void psx_write_half(uint32_t addr, uint16_t val) {
    (void)addr;
    (void)val;
}

int mdec_recently_active(uint32_t within_frames) {
    (void)within_frames;
    return 0;
}

void psx_fatal_halt(const char *reason) {
    (void)reason;
    assert(!"psx_fatal_halt called");
}

void pgxp_set_enabled(int enabled) { (void)enabled; }
int gte_geometry_correction_enabled(void) { return 0; }
int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                            int32_t int_x, int32_t int_y,
                            int32_t *x16, int32_t *y16, uint16_t *sz) {
    (void)addr; (void)packet_word; (void)int_x; (void)int_y;
    (void)x16; (void)y16; (void)sz;
    return PGXP_SRC_NATIVE;
}
int gte_precision_load_word(uint32_t addr, uint32_t packed,
                            int32_t *x16, int32_t *y16, uint16_t *z) {
    (void)addr; (void)packed; (void)x16; (void)y16; (void)z;
    return 0;
}
uint32_t sw_perspective_triangle_count(void) { return 0; }

uint32_t psx_ws_widen_angle_q12(uint32_t vanilla, int extent_pixels) {
    (void)extent_pixels;
    return vanilla;
}
int psx_ws_aspect_cone_contains(int32_t x, int32_t z, int32_t y,
                                int32_t fx, int32_t fz, int32_t fy,
                                uint32_t threshold, int extent_pixels) {
    (void)x; (void)z; (void)y; (void)fx; (void)fz; (void)fy;
    (void)threshold; (void)extent_pixels;
    return 0;
}
void ws_ui_group_assign(WsUiGroupItem *items, size_t count,
                        int32_t display_width, int dense_menu) {
    (void)items; (void)count; (void)display_width; (void)dense_menu;
}
int32_t ws_ui_anchor_for_bounds(int32_t x, int32_t width,
                                int32_t display_width) {
    (void)width;
    if (x * 3 < display_width) return -1;
    if (x * 3 > display_width * 2) return 1;
    return 0;
}

void gpu_vram_dirty_mark_all(void) {}
void gpu_vram_dirty_mark_row_impl(uint32_t y) { (void)y; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
int psx_get_in_exception(void) { return 0; }
int psx_netplay_active(void) { return 0; }
int sio_hold_present_for_card(void) { return 0; }
uint32_t psx_compiled_irq_resume_pc(void) { return 0; }
uint32_t psx_last_irq_check_pc(void) { return 0; }
uint32_t psx_netplay_rb_sticky_bb_pc(void) { return 0; }
void mod_runtime_on_vblank(void) {}
void sio_ape_card_unstick_pump(void) {}
bool screen_kind_from_name(const char *name, ScreenKind *out) {
    (void)name;
    if (out) *out = SCREEN_RAW;
    return true;
}
ColorLut *color_lut_create(const ColorSettings *settings) {
    (void)settings;
    return NULL;
}
void color_lut_destroy(ColorLut *lut) { (void)lut; }
bool color_lut_is_passthrough(const ColorLut *lut) {
    (void)lut;
    return true;
}
void color_lut_map555(const ColorLut *lut, uint16_t bgr555,
                      uint8_t *r, uint8_t *g, uint8_t *b) {
    (void)lut;
    if (r) *r = (uint8_t)((bgr555 & 0x1Fu) << 3);
    if (g) *g = (uint8_t)(((bgr555 >> 5) & 0x1Fu) << 3);
    if (b) *b = (uint8_t)(((bgr555 >> 10) & 0x1Fu) << 3);
}
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address) { return address; }
void text_xlate_vram_upload(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
uint32_t debug_guest_ra(void) { return 0; }
uint32_t debug_guest_sp(void) { return 0; }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)test_ram; }

void gr_init(uint16_t *vram_ptr) { (void)vram_ptr; }
void gr_set_scale(int scale) { (void)scale; }
int gr_scale(void) { return 1; }
void gr_set_texture_filter(int bilinear) { (void)bilinear; }
int gr_texture_filter(void) { return 0; }
void gr_set_semi_transparency(int enabled, int mode) { (void)enabled; (void)mode; }
void gr_set_mask_bits(int set_bit, int check_bit) { (void)set_bit; (void)check_bit; }
void gr_set_texture_window(uint32_t raw) { (void)raw; }
void gr_set_color_modulation(int r, int g, int b, int raw_texture) {
    (void)r; (void)g; (void)b; (void)raw_texture;
}
void gr_set_precise_triangle(int enabled, int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    (void)enabled; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2;
}
void gr_set_perspective_triangle(int enabled, float q0, float q1, float q2) {
    (void)enabled; (void)q0; (void)q1; (void)q2;
}
void gr_fill_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}
void gr_copy_rect(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    (void)src_x; (void)src_y; (void)dst_x; (void)dst_y; (void)w; (void)h;
}
void gr_draw_flat_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
}
void gr_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
    (void)x2; (void)y2; (void)c2;
}
void gr_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage) {
    (void)x0; (void)y0; (void)u0; (void)v0; (void)x1; (void)y1; (void)u1;
    (void)v1; (void)x2; (void)y2; (void)u2; (void)v2; (void)clut_x;
    (void)clut_y; (void)texpage;
}
void gr_draw_shaded_textured_triangle(int x0, int y0, int u0, int v0,
                                      uint32_t color0, int x1, int y1,
                                      int u1, int v1, uint32_t color1,
                                      int x2, int y2, int u2, int v2,
                                      uint32_t color2, uint16_t clut_x,
                                      uint16_t clut_y, uint16_t texpage,
                                      int raw_texture) {
    (void)x0; (void)y0; (void)u0; (void)v0; (void)color0; (void)x1;
    (void)y1; (void)u1; (void)v1; (void)color1; (void)x2; (void)y2;
    (void)u2; (void)v2; (void)color2; (void)clut_x; (void)clut_y;
    (void)texpage; (void)raw_texture;
}
void gr_draw_flat_rect(int x, int y, int w, int h, uint16_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}
void gr_draw_textured_rect(int x, int y, int w, int h, int u, int v,
                           uint16_t clut_x, uint16_t clut_y,
                           uint16_t texpage) {
    last_textured_rect.calls++;
    last_textured_rect.x = x;
    last_textured_rect.y = y;
    last_textured_rect.w = w;
    last_textured_rect.h = h;
    last_textured_rect.u = u;
    last_textured_rect.v = v;
    last_textured_rect.clut_x = clut_x;
    last_textured_rect.clut_y = clut_y;
    last_textured_rect.texpage = texpage;
}
void gr_draw_textured_rect_scaled(int x, int y, int w, int h,
                                  int u0, int v0, int u1, int v1,
                                  uint16_t clut_x, uint16_t clut_y,
                                  uint16_t texpage) {
    (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0; (void)u1;
    (void)v1; (void)clut_x; (void)clut_y; (void)texpage;
}
void gr_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
}
void gr_draw_shaded_line(int x0, int y0, uint16_t c0,
                         int x1, int y1, uint16_t c1) {
    (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1;
}
int gr_render_display(uint32_t *out, int pitch, int dx, int dy, int dw, int dh) {
    (void)out; (void)pitch; (void)dx; (void)dy; (void)dw; (void)dh;
    return 0;
}
int gr_render_display_hires(uint32_t *out, int pitch, int dx, int dy, int dw, int dh) {
    (void)out; (void)pitch; (void)dx; (void)dy; (void)dw; (void)dh;
    return 0;
}
void gr_vram_write(int x, int y, uint16_t pixel) { (void)x; (void)y; (void)pixel; }
uint16_t gr_vram_read(int x, int y) { (void)x; (void)y; return 0; }
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    (void)x; (void)y; (void)w; (void)h; (void)data;
}
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    (void)x; (void)y; (void)w; (void)h; (void)data;
}
void gr_set_draw_area(int x1, int y1, int x2, int y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
void gr_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    if (x1) *x1 = (int)draw_area_left;
    if (y1) *y1 = (int)draw_area_top;
    if (x2) *x2 = (int)draw_area_right;
    if (y2) *y2 = (int)draw_area_bottom;
}
void gr_set_draw_offset(int x, int y) { (void)x; (void)y; }
int gr_wide_supported(void) { return 1; }
void gr_wide_configure(int wide_w, int offset) { (void)wide_w; (void)offset; }
void gr_wide_set_target(int base_x) { (void)base_x; }
void gr_wide_disable_target(void) {}
void gr_wide_clear(int base_x, int y, int h, uint16_t color) {
    (void)base_x; (void)y; (void)h; (void)color;
}
void gr_wide_clear_margins(int base_x, int y, int h, uint16_t color, int sides) {
    (void)base_x; (void)y; (void)h; (void)color; (void)sides;
}
int gr_wide_dump_full(uint32_t *out, int cap_pixels, int *ow, int *oh, int base_x) {
    (void)out; (void)cap_pixels; (void)ow; (void)oh; (void)base_x;
    return 0;
}
int gr_render_wide_display(uint32_t *out, int pitch, int base_x,
                           int disp_y, int disp_h) {
    (void)out; (void)pitch; (void)base_x; (void)disp_y; (void)disp_h;
    return 0;
}
