/* gpu_render_record.c - record software renderer calls for offline replay.
 * See gpu_render_record.h for the file format. */

#include "gpu_render_record.h"
#include "gpu_render.h"
#include "gpu_sw_renderer.h"
#include "psx_file_create_new.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by gpu_render.c. */
const GpuRenderBackend *gr_backend_table(void);
void gr_backend_table_replace(const GpuRenderBackend *table);
int gr_backend_is_software(void);
void gr_record_set_stop_hook(int (*stop)(void));

static FILE *g_file;
static int g_failed;
static const GpuRenderBackend *g_real;
static GpuRenderBackend g_recording;
static uint64_t g_counts[GRR_OP_COUNT];

static void put(const void *data, size_t size) {
    if (g_file && fwrite(data, 1, size, g_file) != size) g_failed = 1;
}

static void op(int code) {
    const uint8_t byte = (uint8_t)code;
    put(&byte, 1);
    g_counts[code]++;
}

static void ints(int count, ...) {
    va_list args;
    va_start(args, count);
    for (int i = 0; i < count; ++i) {
        const int32_t value = (int32_t)va_arg(args, int);
        put(&value, sizeof value);
    }
    va_end(args);
}

static int put_state(void) {
    const size_t size = sw_state_size();
    void *state = malloc(size);
    const uint32_t size32 = (uint32_t)size;
    const int saved = state && sw_state_save(state, size);
    if (saved) {
        put(&size32, sizeof size32);
        put(state, size);
    }
    free(state);
    return saved;
}

/* ---- Recording wrappers: write the call, then forward it ---- */

static void r_init(uint16_t *vram) { op(GRR_UNSUPPORTED); g_real->init(vram); }
static void r_set_scale(int s) { op(GRR_SET_SCALE); ints(1, s); g_real->set_scale(s); }
static void r_set_texture_filter(int on) {
    op(GRR_SET_TEXTURE_FILTER); ints(1, on); g_real->set_texture_filter(on);
}
static void r_set_semi(int on, int mode) {
    op(GRR_SET_SEMI); ints(2, on, mode); g_real->set_semi_transparency(on, mode);
}
static void r_set_mask(int set, int check) {
    op(GRR_SET_MASK); ints(2, set, check); g_real->set_mask_bits(set, check);
}
static void r_set_texwin(uint32_t raw) {
    op(GRR_SET_TEXWIN); put(&raw, sizeof raw); g_real->set_texture_window(raw);
}
static void r_set_color_mod(int r, int g, int b, int raw) {
    op(GRR_SET_COLOR_MOD); ints(4, r, g, b, raw); g_real->set_color_modulation(r, g, b, raw);
}
static void r_set_precise(int on, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          int32_t x2, int32_t y2) {
    op(GRR_SET_PRECISE); ints(7, on, x0, y0, x1, y1, x2, y2);
    g_real->set_precise_triangle(on, x0, y0, x1, y1, x2, y2);
}
static void r_set_perspective(int on, float q0, float q1, float q2) {
    const float q[3] = {q0, q1, q2};
    op(GRR_SET_PERSPECTIVE); ints(1, on); put(q, sizeof q);
    g_real->set_perspective_triangle(on, q0, q1, q2);
}
static void r_fill(int x, int y, int w, int h, uint16_t c) {
    op(GRR_FILL); ints(5, x, y, w, h, c); g_real->fill_rect(x, y, w, h, c);
}
static void r_copy(int sx, int sy, int dx, int dy, int w, int h) {
    op(GRR_COPY); ints(6, sx, sy, dx, dy, w, h); g_real->copy_rect(sx, sy, dx, dy, w, h);
}
static void r_flat_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    op(GRR_FLAT_TRI); ints(7, x0, y0, x1, y1, x2, y2, c);
    g_real->draw_flat_triangle(x0, y0, x1, y1, x2, y2, c);
}
static void r_gouraud_tri(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                          int x2, int y2, uint16_t c2) {
    op(GRR_GOURAUD_TRI); ints(9, x0, y0, c0, x1, y1, c1, x2, y2, c2);
    g_real->draw_gouraud_triangle(x0, y0, c0, x1, y1, c1, x2, y2, c2);
}
static void r_tex_tri(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1,
                      int x2, int y2, int u2, int v2, uint16_t cx, uint16_t cy,
                      uint16_t tp) {
    op(GRR_TEX_TRI); ints(15, x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2, cx, cy, tp);
    g_real->draw_textured_triangle(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2,
                                   cx, cy, tp);
}
static void r_shaded_tex_tri(int x0, int y0, int u0, int v0, uint32_t c0,
                             int x1, int y1, int u1, int v1, uint32_t c1,
                             int x2, int y2, int u2, int v2, uint32_t c2,
                             uint16_t cx, uint16_t cy, uint16_t tp, int raw) {
    op(GRR_SHADED_TEX_TRI);
    ints(19, x0, y0, u0, v0, (int)c0, x1, y1, u1, v1, (int)c1, x2, y2, u2, v2, (int)c2,
         cx, cy, tp, raw);
    g_real->draw_shaded_textured_triangle(x0, y0, u0, v0, c0, x1, y1, u1, v1, c1,
                                          x2, y2, u2, v2, c2, cx, cy, tp, raw);
}
static void r_flat_rect(int x, int y, int w, int h, uint16_t c) {
    op(GRR_FLAT_RECT); ints(5, x, y, w, h, c); g_real->draw_flat_rect(x, y, w, h, c);
}
static void r_tex_rect(int x, int y, int w, int h, int u, int v, uint16_t cx,
                       uint16_t cy, uint16_t tp) {
    op(GRR_TEX_RECT); ints(9, x, y, w, h, u, v, cx, cy, tp);
    g_real->draw_textured_rect(x, y, w, h, u, v, cx, cy, tp);
}
static void r_tex_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1,
                              int v1, uint16_t cx, uint16_t cy, uint16_t tp) {
    op(GRR_TEX_RECT_SCALED); ints(11, x, y, w, h, u0, v0, u1, v1, cx, cy, tp);
    g_real->draw_textured_rect_scaled(x, y, w, h, u0, v0, u1, v1, cx, cy, tp);
}
static void r_line(int x0, int y0, int x1, int y1, uint16_t c) {
    op(GRR_LINE); ints(5, x0, y0, x1, y1, c); g_real->draw_line(x0, y0, x1, y1, c);
}
static void r_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1) {
    op(GRR_SHADED_LINE); ints(6, x0, y0, c0, x1, y1, c1);
    g_real->draw_shaded_line(x0, y0, c0, x1, y1, c1);
}
static int r_render_display(uint32_t *out, int pitch, int dx, int dy, int dw, int dh) {
    const int result = g_real->render_display(out, pitch, dx, dy, dw, dh);
    op(GRR_RENDER_DISPLAY); ints(6, pitch, dx, dy, dw, dh, result);
    return result;
}
static int r_render_display_hires(uint32_t *out, int pitch, int dx, int dy, int dw,
                                  int dh) {
    const int result = g_real->render_display_hires(out, pitch, dx, dy, dw, dh);
    op(GRR_RENDER_DISPLAY_HIRES); ints(6, pitch, dx, dy, dw, dh, result);
    return result;
}
static void r_vram_write(int x, int y, uint16_t pixel) {
    op(GRR_VRAM_WRITE); ints(3, x, y, pixel); g_real->vram_write(x, y, pixel);
}
static uint16_t r_vram_read(int x, int y) {
    const uint16_t result = g_real->vram_read(x, y);
    op(GRR_VRAM_READ); ints(3, x, y, result);
    return result;
}
static void r_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    op(GRR_TRANSFER_IN); ints(4, x, y, w, h);
    if (w > 0 && h > 0) put(data, (size_t)w * h * sizeof *data);
    g_real->vram_transfer_in(x, y, w, h, data);
}
static void r_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    g_real->vram_transfer_out(x, y, w, h, data);
    op(GRR_TRANSFER_OUT); ints(4, x, y, w, h);
    if (w > 0 && h > 0) put(data, (size_t)w * h * sizeof *data);
}
static void r_set_draw_area(int x1, int y1, int x2, int y2) {
    op(GRR_SET_DRAW_AREA); ints(4, x1, y1, x2, y2); g_real->set_draw_area(x1, y1, x2, y2);
}
static void r_get_draw_area(int *x1, int *y1, int *x2, int *y2) {
    g_real->get_draw_area(x1, y1, x2, y2);
    op(GRR_GET_DRAW_AREA); ints(4, *x1, *y1, *x2, *y2);
}
static void r_set_draw_offset(int x, int y) {
    op(GRR_SET_DRAW_OFFSET); ints(2, x, y); g_real->set_draw_offset(x, y);
}
/* The wide compositor keeps surfaces outside the snapshot. */
static void r_wide_configure(int w, int offset) {
    op(GRR_WIDE_CONFIGURE); ints(2, w, offset); g_real->wide_configure(w, offset);
}
static void r_wide_set_view(int on, int shift, int left, int right) {
    op(GRR_WIDE_SET_VIEW); ints(4, on, shift, left, right);
    g_real->wide_set_view(on, shift, left, right);
}
static void r_wide_set_target(int base) {
    op(GRR_WIDE_SET_TARGET); ints(1, base); g_real->wide_set_target(base);
}
static void r_wide_disable_target(void) {
    op(GRR_WIDE_DISABLE_TARGET); g_real->wide_disable_target();
}
static void r_wide_clear(int base, int y, int h, uint16_t c) {
    op(GRR_UNSUPPORTED); g_real->wide_clear(base, y, h, c);
}
static void r_wide_clear_margins(int base, int y, int h, uint16_t c, int sides) {
    op(GRR_UNSUPPORTED); g_real->wide_clear_margins(base, y, h, c, sides);
}
static void r_wide_feedback_rect(int sb, int sx, int sy, int dx, int dy, int w, int h,
                                 int r, int g, int b, int raw, int semi) {
    op(GRR_UNSUPPORTED);
    g_real->wide_feedback_rect(sb, sx, sy, dx, dy, w, h, r, g, b, raw, semi);
}

int gr_record_active(void) { return g_file != NULL; }

uint64_t gr_record_count(int code) {
    return code >= 0 && code < GRR_OP_COUNT ? g_counts[code] : 0;
}

int gr_record_start(const char *path) {
    if (g_file || !gr_backend_is_software()) return 0;
    g_file = psx_file_create_new(path, 1);
    if (!g_file) return 0;
    setvbuf(g_file, NULL, _IOFBF, 1 << 20);
    g_failed = 0;
    memset(g_counts, 0, sizeof g_counts);
    const uint32_t version = GRR_VERSION;
    put(GRR_MAGIC, 8);
    put(&version, sizeof version);
    if (!put_state() || g_failed) {
        fclose(g_file);
        g_file = NULL;
        remove(path);
        return 0;
    }
    g_real = gr_backend_table();
    g_recording = *g_real;
#define WRAP(field, fn) if (g_real->field) g_recording.field = fn
    WRAP(init, r_init);
    WRAP(set_scale, r_set_scale);
    WRAP(set_texture_filter, r_set_texture_filter);
    WRAP(set_semi_transparency, r_set_semi);
    WRAP(set_mask_bits, r_set_mask);
    WRAP(set_texture_window, r_set_texwin);
    WRAP(set_color_modulation, r_set_color_mod);
    WRAP(set_precise_triangle, r_set_precise);
    WRAP(set_perspective_triangle, r_set_perspective);
    WRAP(fill_rect, r_fill);
    WRAP(copy_rect, r_copy);
    WRAP(draw_flat_triangle, r_flat_tri);
    WRAP(draw_gouraud_triangle, r_gouraud_tri);
    WRAP(draw_textured_triangle, r_tex_tri);
    WRAP(draw_shaded_textured_triangle, r_shaded_tex_tri);
    WRAP(draw_flat_rect, r_flat_rect);
    WRAP(draw_textured_rect, r_tex_rect);
    WRAP(draw_textured_rect_scaled, r_tex_rect_scaled);
    WRAP(draw_line, r_line);
    WRAP(draw_shaded_line, r_shaded_line);
    WRAP(render_display, r_render_display);
    WRAP(render_display_hires, r_render_display_hires);
    WRAP(vram_write, r_vram_write);
    WRAP(vram_read, r_vram_read);
    WRAP(vram_transfer_in, r_transfer_in);
    WRAP(vram_transfer_out, r_transfer_out);
    WRAP(set_draw_area, r_set_draw_area);
    WRAP(get_draw_area, r_get_draw_area);
    WRAP(set_draw_offset, r_set_draw_offset);
    WRAP(wide_configure, r_wide_configure);
    WRAP(wide_set_view, r_wide_set_view);
    WRAP(wide_set_target, r_wide_set_target);
    WRAP(wide_disable_target, r_wide_disable_target);
    WRAP(wide_clear, r_wide_clear);
    WRAP(wide_clear_margins, r_wide_clear_margins);
    WRAP(wide_feedback_rect, r_wide_feedback_rect);
#undef WRAP
    gr_backend_table_replace(&g_recording);
    gr_record_set_stop_hook(gr_record_stop);
    return 1;
}

int gr_record_stop(void) {
    if (!g_file) return 0;
    gr_backend_table_replace(g_real);
    gr_record_set_stop_hook(NULL);
    op(GRR_END);
    const int saved = put_state();
    const int closed = fclose(g_file) == 0;
    g_file = NULL;
    return saved && closed && !g_failed;
}
