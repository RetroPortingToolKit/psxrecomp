/* Shared link stubs for tests that compile runtime/src/gpu.c directly and
 * execute GP0 packets without a GL context, mod bank or emulator core.
 * Include right after "../src/gpu.c". Define GPU_EXEC_REAL_UI_GROUP before
 * including to link the real ws_ui_group.c instead of the grouping stub.
 * Draw calls that tests observe are recorded below. */

uint64_t s_frame_count;
uint64_t psx_cycle_count;          /* gpu_timeline.c stamps notes with it */
int g_exec_phase;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
CPUState *debug_cpu_ptr;
int g_psx_vram_dirty_tracking;
uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;

/* This fixture exercises stock GPU packets without a GL context or mod bank.
 * Keep the gpu.c dependencies explicit, including on linkers that retain
 * additional exported command handlers despite --gc-sections. */
static GrBackend fixture_backend = GR_BACKEND_SOFTWARE;
GrBackend gr_backend(void) { return fixture_backend; }
int gl_renderer_texture_banks_supported(void) { return 0; }
static int fixture_projective_supported, fixture_projective_draws;
static PSXProjectedVertex fixture_projective_vertices[3];
int gl_renderer_projective_supported(void) { return fixture_projective_supported; }
void gl_renderer_draw_projected_triangle(const PSXProjectedVertex v[3],
    uint16_t tp, uint16_t cx, uint16_t cy, int raw, int semi, int perspective) {
    memcpy(fixture_projective_vertices,v,sizeof fixture_projective_vertices);
    fixture_projective_draws++;
    (void)tp;(void)cx;(void)cy;(void)raw;(void)semi;(void)perspective;
}
static struct { uint32_t addr,word; PGXPProjection p; int valid; } fixture_homogeneous[4];
int pgxp_load_projection(uint32_t addr, uint32_t word, PGXPProjection *out) {
    for(int i=0;i<4;++i) if(fixture_homogeneous[i].valid &&
        fixture_homogeneous[i].addr==addr && fixture_homogeneous[i].word==word) {
        if(out)*out=fixture_homogeneous[i].p;
        return 1;
    }
    return 0;
}
static int fixture_projection_tracking;
int pgxp_projection_tracking(void) { return fixture_projection_tracking; }
void pgxp_set_projection_tracking(int enabled) { fixture_projection_tracking=!!enabled; }
int gl_renderer_select_texture_bank(uint16_t id) { return id == 0; }
int gl_renderer_select_texture_bank_live_clut(uint16_t id) { return id == 0; }
uint16_t mod_texture_packet_bank(uint32_t source, const uint32_t *words,
                                 uint32_t count) {
    (void)source; (void)words; (void)count;
    return 0;
}
int mod_texture_packet_precision(uint32_t source, float q[3], float xy[6]) {
    (void)source; (void)q; (void)xy;
    return 0;
}

static uint32_t test_ram[0x00200000u / 4u];
/* The enhancement GPU-DMA aperture (mod_memory.h): a test allocates it by
 * setting test_aperture_used; reads and keys then follow the real rules. */
static uint32_t test_aperture[0x00020000u / 4u];
static uint32_t test_aperture_used;

static struct {
    int calls;
    int x, y, w, h;
    int u, v;
    uint16_t clut_x, clut_y, texpage;
} last_textured_rect;
static struct { int calls, x, y, w, h, u0, v0, u1, v1; } last_scaled_rect;

/* PGXP stub state: the quad case in main() switches geometry correction on
 * and makes the packet words in a bitmask (bit n = word n at g_test_quad_base)
 * report a dataflow-precise vertex. */
static int g_test_geometry_correction = 0;
static uint32_t g_test_quad_base = 0xFFFFFFFFu;
static uint32_t g_test_precise_words = 0;
static int g_test_rect_bypass = 0;
static int g_test_rect_partial = 0;

static int test_word_precise(uint32_t addr) {
    if (g_test_quad_base == 0xFFFFFFFFu || addr < g_test_quad_base) return 0;
    uint32_t word = (addr - g_test_quad_base) / 4u;
    return word < 32u && ((g_test_precise_words >> word) & 1u);
}
static int g_test_precise_triangles = 0;
static int g_test_textured_triangles = 0;

/* Bounding box of every flat/gouraud triangle drawn since the last reset. */
static struct { int calls, min_x, max_x, min_y, max_y; } gpu_exec_triangles;
static inline void gpu_exec_reset_triangles(void) {
    gpu_exec_triangles.calls = 0;
    gpu_exec_triangles.min_x = gpu_exec_triangles.min_y = 1 << 30;
    gpu_exec_triangles.max_x = gpu_exec_triangles.max_y = -(1 << 30);
}
static inline void gpu_exec_record_triangle(int x0, int y0, int x1, int y1,
                                     int x2, int y2) {
    const int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    gpu_exec_triangles.calls++;
    for (int i = 0; i < 3; i++) {
        if (xs[i] < gpu_exec_triangles.min_x) gpu_exec_triangles.min_x = xs[i];
        if (xs[i] > gpu_exec_triangles.max_x) gpu_exec_triangles.max_x = xs[i];
        if (ys[i] < gpu_exec_triangles.min_y) gpu_exec_triangles.min_y = ys[i];
        if (ys[i] > gpu_exec_triangles.max_y) gpu_exec_triangles.max_y = ys[i];
    }
}

uint32_t psx_read_word(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu, off;
    if (psx_mod_gpu_dma_aperture_offset_for(phys, 4u, test_aperture_used, &off))
        return test_aperture[off / 4u];
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

void psx_host_write_half(uint32_t addr, uint16_t val) {
    psx_write_half(addr, val);
}

int mdec_recently_active(uint32_t within_frames) {
    (void)within_frames;
    return 0;
}

void psx_fatal_halt(const char *reason) {
    (void)reason;
    assert(!"psx_fatal_halt called");
}

static int fixture_pgxp_enabled;
void pgxp_set_enabled(int enabled) { fixture_pgxp_enabled = enabled; }
int gte_geometry_correction_enabled(void) { return g_test_geometry_correction; }
int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                            int32_t int_x, int32_t int_y,
                            int32_t *x16, int32_t *y16, uint16_t *sz) {
    (void)packet_word;
    *x16 = int_x * 65536;
    *y16 = int_y * 65536;
    *sz = 0;
    if (test_word_precise(addr)) {
        *x16 += 0x4000;
        return PGXP_SRC_DATAFLOW;
    }
    return PGXP_SRC_NATIVE;
}
static struct {
    uint32_t addr, word;
    int32_t x16, y16;
    uint16_t z;
    int valid;
} fixture_projections[4];
int pgxp_probe_precise_vertex(uint32_t addr, uint32_t packet_word,
                              int32_t int_x, int32_t int_y) {
    (void)packet_word; (void)int_x; (void)int_y;
    return test_word_precise(addr) ? PGXP_SRC_DATAFLOW : PGXP_SRC_NATIVE;
}
void pgxp_note_triangle(int precise) { (void)precise; }
void pgxp_note_triangle_detail(const PGXPTriRecord *rec) { (void)rec; }
void pgxp_note_rect_bypass(int all_precise) {
    if (all_precise) g_test_rect_bypass++;
    else             g_test_rect_partial++;
}
int gte_precision_load_word(uint32_t addr, uint32_t packed,
                            int32_t *x16, int32_t *y16, uint16_t *z) {
    for (unsigned i = 0; i < 4; ++i) {
        if (fixture_projections[i].valid && fixture_projections[i].addr == addr &&
            fixture_projections[i].word == packed) {
            if (x16) *x16 = fixture_projections[i].x16;
            if (y16) *y16 = fixture_projections[i].y16;
            if (z) *z = fixture_projections[i].z;
            return fixture_projections[i].z != 0;
        }
    }
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
#ifndef GPU_EXEC_REAL_UI_GROUP
void ws_ui_group_assign(WsUiGroupItem *items, size_t count,
                        int32_t display_width, int dense_menu, int in_place) {
    (void)items; (void)count; (void)display_width; (void)dense_menu;
    (void)in_place;
}
int32_t ws_ui_anchor_for_bounds(int32_t x, int32_t width,
                                int32_t display_width) {
    (void)width;
    if (x * 3 < display_width) return -1;
    if (x * 3 > display_width * 2) return 1;
    return 0;
}
#endif

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
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t address) {
    return psx_mod_gpu_dma_resolve_address_for(address, test_aperture_used);
}
int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t bytes) {
    return bytes && psx_mod_gpu_dma_aperture_offset_for(
        address, bytes, test_aperture_used, NULL);
}
uint32_t psx_gpu_packet_key(uint32_t address) {
    return psx_gpu_packet_key_for(address, test_aperture_used);
}
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
static struct { int enabled; int32_t xy[6]; } fixture_precise_triangle;
void gr_set_precise_triangle(int enabled, int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    fixture_precise_triangle.enabled = enabled;
    int32_t xy[6] = { x0, y0, x1, y1, x2, y2 };
    memcpy(fixture_precise_triangle.xy, xy, sizeof xy);
    if (enabled) g_test_precise_triangles++;
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
    gpu_exec_record_triangle(x0, y0, x1, y1, x2, y2);
    (void)color;
}
void gr_draw_gouraud_triangle(int x0, int y0, uint16_t c0,
                              int x1, int y1, uint16_t c1,
                              int x2, int y2, uint16_t c2) {
    gpu_exec_record_triangle(x0, y0, x1, y1, x2, y2);
    (void)c0; (void)c1; (void)c2;
}
void gr_draw_textured_triangle(int x0, int y0, int u0, int v0,
                               int x1, int y1, int u1, int v1,
                               int x2, int y2, int u2, int v2,
                               uint16_t clut_x, uint16_t clut_y,
                               uint16_t texpage) {
    g_test_textured_triangles++;
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
#ifdef GPU_EXEC_RECORD_SHADED_TEXTURED
    gpu_exec_record_triangle(x0, y0, x1, y1, x2, y2);
#endif
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
    last_scaled_rect.calls++;
    last_scaled_rect.x=x; last_scaled_rect.y=y;
    last_scaled_rect.w=w; last_scaled_rect.h=h;
    last_scaled_rect.u0=u0; last_scaled_rect.v0=v0;
    last_scaled_rect.u1=u1; last_scaled_rect.v1=v1;
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
void gr_vram_upload_begin(int x,int y,int w,int h) { (void)x; (void)y; (void)w; (void)h; }
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
static struct { int enabled, shift, left, right; } fixture_wide_view;
void gr_wide_set_view(int enabled, int shift, int left, int right) {
    fixture_wide_view.enabled = enabled;
    fixture_wide_view.shift = shift;
    fixture_wide_view.left = left;
    fixture_wide_view.right = right;
}
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
int gte_nclip_native_wide_sign(int32_t mac0, int* sign) {
    (void)mac0; (void)sign; return 0;
}
int gte_nclip_native_wide_previous_sign(int32_t mac0, int* sign) {
    (void)mac0; (void)sign; return 0;
}
static int fixture_triangle_recovered;
void gl_renderer_note_wide_triangle_recovery(int enabled) {
    fixture_triangle_recovered=enabled;
}
/* Smooth motion (frame generation) vertex sources: off in these fixtures. */
int gl_renderer_frame_generation(void) { return 0; }
void gl_renderer_fg_source(const uint32_t id[3], const int32_t pc[9], const int32_t h[3],
                           const int32_t x[3], const int32_t y[3]) {
    (void)id; (void)pc; (void)h; (void)x; (void)y;
}
void gte_fg_source_set(int enabled) { (void)enabled; }
int gte_fg_source_enabled(void) { return 0; }
int gte_fg_source_lookup(uint32_t packed, GteFgSrc out[2]) { (void)packed; (void)out; return 0; }
