/* Replays renderer recordings (gpu_render_record.h) through the software
 * renderer.
 *
 *   gr_replay [--compare-only] RECORDING...
 *
 * Pass 1 must reproduce every recorded read, draw-area query and VRAM
 * readback, and end on the recording's closing snapshot exactly. A recording
 * that passes is a regression capture: after a software renderer change, a
 * mismatch shows exactly which op changed the output.
 * Pass 2 replays again and hashes each op's observable output (reads,
 * readbacks, decoded display images) and the dirty rows. Both must match
 * pass 1 at every op, and the final state byte for byte, so the replay is
 * deterministic. A second renderer implementation can run as pass 2 to be
 * compared op by op against the software reference.
 * --compare-only skips the checks against recorded results, for generated
 * recordings that hold placeholders. Exit status 0 means all passed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_render_record.h"
#include "gpu_sw_renderer.h"

enum { VRAM_W = 1024, VRAM_H = 512, MAX_SCALE = 4 };

/* Link stubs for the renderer's dependencies outside the software backend. */
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
int g_psx_vram_dirty_tracking = 1;
static unsigned char g_dirty[VRAM_H];
void gpu_vram_dirty_mark_row_impl(uint32_t y) { if (y < VRAM_H) g_dirty[y] = 1; }
void gpu_vram_dirty_mark_rect(int x, int y, int w, int h) {
    (void)x; (void)w;
    for (int row = y; row < y + h; ++row) g_dirty[row & (VRAM_H - 1)] = 1;
}
void gpu_vram_dirty_mark_all(void) { memset(g_dirty, 1, sizeof g_dirty); }

static uint16_t g_vram[VRAM_W * VRAM_H];
/* --compare-only: generated recordings hold placeholder results, so only
 * the two passes are compared with each other. */
static int g_compare_only;

typedef struct {
    const uint8_t *p, *end;
    int bad;
} Reader;

static void take(Reader *r, void *out, size_t size) {
    if ((size_t)(r->end - r->p) < size) {
        r->bad = 1;
        memset(out, 0, size);
        return;
    }
    memcpy(out, r->p, size);
    r->p += size;
}

static int32_t i32(Reader *r) { int32_t v; take(r, &v, sizeof v); return v; }

static uint64_t fnv(uint64_t h, const void *data, size_t size) {
    const uint8_t *b = data;
    for (size_t i = 0; i < size; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

typedef struct {
    uint64_t *hashes;      /* one per op */
    size_t ops;
    void *final_state;
    size_t final_size;
    unsigned char dirty[VRAM_H];
    uint64_t draws;
} Pass;

static const char *const k_names[GRR_OP_COUNT] = {
    "END", "SET_SCALE", "SET_TEXTURE_FILTER", "SET_SEMI", "SET_MASK", "SET_TEXWIN",
    "SET_COLOR_MOD", "SET_PRECISE", "SET_PERSPECTIVE", "FILL", "COPY", "FLAT_TRI",
    "GOURAUD_TRI", "TEX_TRI", "SHADED_TEX_TRI", "FLAT_RECT", "TEX_RECT",
    "TEX_RECT_SCALED", "LINE", "SHADED_LINE", "RENDER_DISPLAY",
    "RENDER_DISPLAY_HIRES", "VRAM_WRITE", "VRAM_READ", "TRANSFER_IN",
    "TRANSFER_OUT", "SET_DRAW_AREA", "GET_DRAW_AREA", "SET_DRAW_OFFSET",
    "UNSUPPORTED", "WIDE_CONFIGURE",
    "WIDE_SET_VIEW", "WIDE_SET_TARGET", "WIDE_DISABLE_TARGET",
};

/* Reports where two snapshots differ: settings, native VRAM or the mirror. */
static void describe_difference(const uint8_t *a, const uint8_t *b, size_t size) {
    int32_t scale;
    memcpy(&scale, a + 8, sizeof scale);
    const size_t native = (size_t)VRAM_W * VRAM_H * 2;
    const size_t mirror = scale > 1 ? native * (size_t)scale * (size_t)scale : 0;
    const size_t header = size - native - mirror;
    if (memcmp(a, b, header) != 0) fprintf(stderr, "  renderer settings differ\n");
    const struct { const char *name; size_t offset, bytes; int width; } regions[] = {
        {"native VRAM", header, native, VRAM_W},
        {"high-resolution mirror", header + native, mirror, VRAM_W * scale},
    };
    for (int r = 0; r < 2; ++r) {
        const uint16_t *pa = (const uint16_t *)(a + regions[r].offset);
        const uint16_t *pb = (const uint16_t *)(b + regions[r].offset);
        size_t count = 0, first = 0;
        int min_y = INT32_MAX, max_y = -1;
        for (size_t i = 0; i < regions[r].bytes / 2; ++i)
            if (pa[i] != pb[i]) {
                if (!count++) first = i;
                const int y = (int)(i / (size_t)regions[r].width);
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        if (count)
            fprintf(stderr, "  %s: %zu pixels differ, first (%zu,%zu) replay %04x recorded %04x,"
                    " rows %d-%d\n", regions[r].name, count, first % (size_t)regions[r].width,
                    first / (size_t)regions[r].width, pa[first], pb[first], min_y, max_y);
    }
}

/* Runs one pass. Returns 0 and prints the reason on any failure. */
static int run(const char *name, const uint8_t *data, size_t size, int second,
               Pass *pass, uint64_t counts[GRR_OP_COUNT]) {
    Reader r = {data, data + size, 0};
    char magic[8];
    take(&r, magic, sizeof magic);
    const uint32_t version = (uint32_t)i32(&r);
    const uint32_t state_size = (uint32_t)i32(&r);
    if (r.bad || memcmp(magic, GRR_MAGIC, 8) != 0 || version != GRR_VERSION ||
        (size_t)(r.end - r.p) < state_size) {
        fprintf(stderr, "%s: not a version %u renderer recording\n", name, GRR_VERSION);
        return 0;
    }
    sw_renderer_init(g_vram);
    if (!sw_state_load(r.p, state_size)) {
        fprintf(stderr, "%s: opening snapshot does not load\n", name);
        return 0;
    }
    r.p += state_size;
    memset(g_dirty, 0, sizeof g_dirty);

    size_t capacity = 1 << 16;
    pass->hashes = malloc(capacity * sizeof *pass->hashes);
    pass->ops = 0;
    uint32_t *display = malloc((size_t)VRAM_W * MAX_SCALE * VRAM_H * MAX_SCALE * 4);
    uint16_t *pixels = malloc((size_t)VRAM_W * VRAM_H * sizeof *pixels);
    int ok = pass->hashes && display && pixels;
    while (ok) {
        uint8_t code;
        take(&r, &code, 1);
        if (r.bad || code >= GRR_OP_COUNT) {
            fprintf(stderr, "%s: truncated or unknown op at byte %td\n", name, r.p - data);
            ok = 0;
            break;
        }
        if (code == GRR_END) break;
        if (code == GRR_UNSUPPORTED) {
            fprintf(stderr, "%s: recording used the wide compositor or re-initialized\n", name);
            ok = 0;
            break;
        }
        if (counts) counts[code]++;
        uint64_t h = 1469598103934665603ull ^ code;
        int32_t a[20];
        switch (code) {
        case GRR_SET_SCALE: sw_renderer_set_scale(i32(&r)); break;
        case GRR_SET_TEXTURE_FILTER: sw_set_texture_filter(i32(&r)); break;
        case GRR_SET_SEMI: a[0] = i32(&r); a[1] = i32(&r);
            sw_set_semi_transparency(a[0], a[1]); break;
        case GRR_SET_MASK: a[0] = i32(&r); a[1] = i32(&r); sw_set_mask_bits(a[0], a[1]); break;
        case GRR_SET_TEXWIN: sw_set_texture_window((uint32_t)i32(&r)); break;
        case GRR_SET_COLOR_MOD:
            for (int i = 0; i < 4; ++i) a[i] = i32(&r);
            sw_set_color_modulation(a[0], a[1], a[2], a[3]); break;
        case GRR_SET_PRECISE:
            for (int i = 0; i < 7; ++i) a[i] = i32(&r);
            sw_set_precise_triangle(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case GRR_SET_PERSPECTIVE: {
            float q[3];
            a[0] = i32(&r);
            take(&r, q, sizeof q);
            sw_set_perspective_triangle(a[0], q[0], q[1], q[2]);
            break;
        }
        case GRR_FILL:
            for (int i = 0; i < 5; ++i) a[i] = i32(&r);
            sw_fill_rect(a[0], a[1], a[2], a[3], (uint16_t)a[4]); break;
        case GRR_COPY:
            for (int i = 0; i < 6; ++i) a[i] = i32(&r);
            sw_copy_rect(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case GRR_FLAT_TRI:
            for (int i = 0; i < 7; ++i) a[i] = i32(&r);
            sw_draw_flat_triangle(a[0], a[1], a[2], a[3], a[4], a[5], (uint16_t)a[6]); break;
        case GRR_GOURAUD_TRI:
            for (int i = 0; i < 9; ++i) a[i] = i32(&r);
            sw_draw_gouraud_triangle(a[0], a[1], (uint16_t)a[2], a[3], a[4], (uint16_t)a[5],
                                     a[6], a[7], (uint16_t)a[8]); break;
        case GRR_TEX_TRI:
            for (int i = 0; i < 15; ++i) a[i] = i32(&r);
            sw_draw_textured_triangle(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                      a[8], a[9], a[10], a[11], (uint16_t)a[12],
                                      (uint16_t)a[13], (uint16_t)a[14]); break;
        case GRR_SHADED_TEX_TRI:
            for (int i = 0; i < 19; ++i) a[i] = i32(&r);
            sw_draw_shaded_textured_triangle(a[0], a[1], a[2], a[3], (uint32_t)a[4],
                a[5], a[6], a[7], a[8], (uint32_t)a[9], a[10], a[11], a[12], a[13],
                (uint32_t)a[14], (uint16_t)a[15], (uint16_t)a[16], (uint16_t)a[17], a[18]);
            break;
        case GRR_FLAT_RECT:
            for (int i = 0; i < 5; ++i) a[i] = i32(&r);
            sw_draw_flat_rect(a[0], a[1], a[2], a[3], (uint16_t)a[4]); break;
        case GRR_TEX_RECT:
            for (int i = 0; i < 9; ++i) a[i] = i32(&r);
            sw_draw_textured_rect(a[0], a[1], a[2], a[3], a[4], a[5], (uint16_t)a[6],
                                  (uint16_t)a[7], (uint16_t)a[8]); break;
        case GRR_TEX_RECT_SCALED:
            for (int i = 0; i < 11; ++i) a[i] = i32(&r);
            sw_draw_textured_rect_scaled(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                         (uint16_t)a[8], (uint16_t)a[9], (uint16_t)a[10]);
            break;
        case GRR_LINE:
            for (int i = 0; i < 5; ++i) a[i] = i32(&r);
            sw_draw_line(a[0], a[1], a[2], a[3], (uint16_t)a[4]); break;
        case GRR_SHADED_LINE:
            for (int i = 0; i < 6; ++i) a[i] = i32(&r);
            sw_draw_shaded_line(a[0], a[1], (uint16_t)a[2], a[3], a[4], (uint16_t)a[5]); break;
        case GRR_RENDER_DISPLAY:
        case GRR_RENDER_DISPLAY_HIRES: {
            for (int i = 0; i < 6; ++i) a[i] = i32(&r);
            const int hires = code == GRR_RENDER_DISPLAY_HIRES;
            const size_t rows = (size_t)(a[4] > 0 ? a[4] : 0) * (hires ? MAX_SCALE : 1);
            const size_t bytes = (size_t)(a[0] > 0 ? a[0] : 0) * rows;
            if (bytes > (size_t)VRAM_W * MAX_SCALE * VRAM_H * MAX_SCALE * 4) {
                fprintf(stderr, "%s: display request too large\n", name);
                ok = 0;
                break;
            }
            memset(display, 0, bytes);
            const int result = hires
                ? sw_render_display_hires(display, a[0], a[1], a[2], a[3], a[4])
                : sw_render_display(display, a[0], a[1], a[2], a[3], a[4]);
            if (!g_compare_only && result != a[5]) {
                fprintf(stderr, "%s: op %zu %s returned %d, recorded %d\n", name, pass->ops,
                        k_names[code], result, a[5]);
                ok = 0;
            }
            h = fnv(h, display, bytes);
            break;
        }
        case GRR_VRAM_WRITE:
            for (int i = 0; i < 3; ++i) a[i] = i32(&r);
            sw_vram_write(a[0], a[1], (uint16_t)a[2]); break;
        case GRR_VRAM_READ: {
            for (int i = 0; i < 3; ++i) a[i] = i32(&r);
            const uint16_t value = sw_vram_read(a[0], a[1]);
            if (!g_compare_only && value != (uint16_t)a[2]) {
                fprintf(stderr, "%s: op %zu VRAM_READ(%d,%d)=%04x, recorded %04x\n", name,
                        pass->ops, a[0], a[1], value, (uint16_t)a[2]);
                ok = 0;
            }
            h = fnv(h, &value, sizeof value);
            break;
        }
        case GRR_TRANSFER_IN:
        case GRR_TRANSFER_OUT: {
            for (int i = 0; i < 4; ++i) a[i] = i32(&r);
            const size_t count = a[2] > 0 && a[3] > 0 ? (size_t)a[2] * (size_t)a[3] : 0;
            if (count > (size_t)VRAM_W * VRAM_H) { ok = 0; r.bad = 1; break; }
            take(&r, pixels, count * sizeof *pixels);
            if (code == GRR_TRANSFER_IN) {
                sw_vram_transfer_in(a[0], a[1], a[2], a[3], pixels);
            } else {
                uint16_t *out = malloc(count * sizeof *out + 1);
                sw_vram_transfer_out(a[0], a[1], a[2], a[3], out);
                if (!g_compare_only && memcmp(out, pixels, count * sizeof *out) != 0) {
                    fprintf(stderr, "%s: op %zu TRANSFER_OUT differs from the recording\n",
                            name, pass->ops);
                    ok = 0;
                }
                h = fnv(h, out, count * sizeof *out);
                free(out);
            }
            break;
        }
        case GRR_SET_DRAW_AREA:
            for (int i = 0; i < 4; ++i) a[i] = i32(&r);
            sw_set_draw_area(a[0], a[1], a[2], a[3]); break;
        case GRR_GET_DRAW_AREA: {
            int area[4];
            for (int i = 0; i < 4; ++i) a[i] = i32(&r);
            sw_get_draw_area(&area[0], &area[1], &area[2], &area[3]);
            h = fnv(h, area, sizeof area);
            if (!g_compare_only &&
                (area[0] != a[0] || area[1] != a[1] || area[2] != a[2] || area[3] != a[3])) {
                fprintf(stderr, "%s: op %zu GET_DRAW_AREA differs\n", name, pass->ops);
                ok = 0;
            }
            break;
        }
        case GRR_SET_DRAW_OFFSET: a[0] = i32(&r); a[1] = i32(&r);
            sw_set_draw_offset(a[0], a[1]); break;
        /* Wide surfaces are not in the snapshot. Configuring them makes the
         * closing snapshot fail to save, which rejects the recording. */
        case GRR_WIDE_CONFIGURE: a[0] = i32(&r); a[1] = i32(&r);
            sw_wide_configure(a[0], a[1]); break;
        case GRR_WIDE_SET_VIEW:
            for (int i = 0; i < 4; ++i) a[i] = i32(&r);
            sw_wide_set_view(a[0], a[1], a[2], a[3]); break;
        case GRR_WIDE_SET_TARGET: sw_wide_set_target(i32(&r)); break;
        case GRR_WIDE_DISABLE_TARGET: sw_wide_disable_target(); break;
        default:
            fprintf(stderr, "%s: unhandled op %s\n", name, k_names[code]);
            ok = 0;
        }
        if (r.bad) {
            fprintf(stderr, "%s: truncated op %s\n", name, k_names[code]);
            ok = 0;
        }
        if (!ok) break;
        if (pass->ops == capacity) {
            capacity *= 2;
            uint64_t *grown = realloc(pass->hashes, capacity * sizeof *grown);
            if (!grown) { ok = 0; break; }
            pass->hashes = grown;
        }
        pass->hashes[pass->ops++] = h;
    }
    if (ok) {
        const uint32_t closing_size = (uint32_t)i32(&r);
        pass->final_size = sw_state_size();
        pass->final_state = malloc(pass->final_size);
        /* Only --compare-only accepts generated recordings without an END state. */
        if (r.bad || (!g_compare_only && !closing_size) ||
            (closing_size && closing_size != pass->final_size) ||
            (size_t)(r.end - r.p) != closing_size ||
            !pass->final_state || !sw_state_save(pass->final_state, pass->final_size)) {
            fprintf(stderr, "%s: invalid closing snapshot or trailing data\n", name);
            ok = 0;
        } else if (!second && closing_size &&
                   memcmp(pass->final_state, r.p, closing_size) != 0) {
            fprintf(stderr, "%s: CPU replay does not reproduce the closing snapshot; "
                    "the recording missed a VRAM change\n", name);
            describe_difference(pass->final_state, r.p, closing_size);
            ok = 0;
        }
        memcpy(pass->dirty, g_dirty, sizeof g_dirty);
    }
    free(display);
    free(pixels);
    return ok;
}

int main(int argc, char **argv) {
    int first_file = 1;
    if (argc > 1 && strcmp(argv[1], "--compare-only") == 0) {
        g_compare_only = 1;
        first_file = 2;
    }
    if (argc <= first_file) {
        fprintf(stderr, "usage: %s [--compare-only] RECORDING...\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (int f = first_file; f < argc; ++f) {
        FILE *file = fopen(argv[f], "rb");
        if (!file) { perror(argv[f]); ++failures; continue; }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        uint8_t *data = malloc((size_t)size);
        const int read_ok = data && fread(data, 1, (size_t)size, file) == (size_t)size;
        fclose(file);
        uint64_t counts[GRR_OP_COUNT] = {0};
        Pass first_pass = {0}, second_pass = {0};
        int ok = read_ok && run(argv[f], data, (size_t)size, 0, &first_pass, counts);
        ok = ok && run(argv[f], data, (size_t)size, 1, &second_pass, NULL);
        if (ok) {
            size_t first = first_pass.ops == second_pass.ops ? SIZE_MAX : 0;
            for (size_t i = 0; i < first_pass.ops && i < second_pass.ops; ++i)
                if (first_pass.hashes[i] != second_pass.hashes[i]) { first = i; break; }
            if (first != SIZE_MAX) {
                fprintf(stderr, "%s: second pass first differs at op %zu\n", argv[f], first);
                ok = 0;
            } else if (first_pass.final_size != second_pass.final_size ||
                       memcmp(first_pass.final_state, second_pass.final_state, first_pass.final_size) != 0) {
                fprintf(stderr, "%s: second pass final VRAM or mirror differs\n", argv[f]);
                ok = 0;
            } else if (memcmp(first_pass.dirty, second_pass.dirty, sizeof first_pass.dirty) != 0) {
                fprintf(stderr, "%s: second pass dirty rows differ\n", argv[f]);
                ok = 0;
            }
        }
        printf("%s: %s, %zu ops (", argv[f], ok ? "PASS" : "FAIL", first_pass.ops);
        int first = 1;
        for (int op = 1; op < GRR_OP_COUNT; ++op)
            if (counts[op]) {
                printf("%s%s %llu", first ? "" : ", ", k_names[op], (unsigned long long)counts[op]);
                first = 0;
            }
        printf(")\n");
        failures += !ok;
        free(first_pass.hashes); free(second_pass.hashes);
        free(first_pass.final_state); free(second_pass.final_state);
        free(data);
    }
    return failures ? 1 : 0;
}
