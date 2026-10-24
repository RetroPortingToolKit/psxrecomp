/* gpu_fmv_hd.c -- HD replacement of FMV movies (see gpu_fmv_hd.h).
 *
 * Recognition: a sig_w x sig_h luminance signature of the presented 24-bit frame,
 * compared (mean absolute difference) around the last recognised frame first, then over
 * the whole pack when tracking is lost. Decoding: worker threads read and decode the
 * JPEG/PNG of the upcoming frames ahead into a small ring of RGBA images.
 */
#include "gpu_fmv_hd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "psx_sdl.h"

#define STBI_NO_STDIO
#include "../third_party/stb_image.h"

#ifdef PSX_FMV_HD_TESTING
extern void *fmv_hd_test_malloc(size_t);
extern void *fmv_hd_test_realloc(void *, size_t);
extern SDL_Thread *fmv_hd_test_start_thread(SDL_ThreadFunction, void *);
#define malloc fmv_hd_test_malloc
#define realloc fmv_hd_test_realloc
#endif

#define FMV_MAX_MOVIES   16
#define FMV_RING         32
#define FMV_WORKERS      6
#define FMV_FILE_MAX     (64u * 1024u * 1024u)
#define FMV_DECODE_MAX   (256u * 1024u * 1024u)
#define FMV_HOLD_S       0.5    /* hold of the last HD frame (not recognised / not ready) */
#define FMV_LOCAL_BACK   3
#define FMV_LOCAL_AHEAD  16
#define FMV_ACCEPT_LOCAL 14     /* mean difference accepted near the last frame */
#define FMV_ACCEPT_GLOBAL 9     /* mean difference accepted in a global search */
#define FMV_NEAR_GLOBAL  27     /* difference under which a vertical re-alignment is tried */

typedef struct Movie {
    char dir[1024];
    FmvHdIndexHeader h;
    uint8_t *sigs;              /* frames * sig_w * sig_h */
    int dy;                     /* learnt vertical offset (native lines) */
} Movie;

typedef struct Slot {
    int movie, frame;           /* frame = HD index (source * sub + k); -1 free */
    int state;                  /* 0 free, 1 requested, 2 ready, 3 failed */
    uint8_t *rgba;
    int w, h;
    uint64_t used;
    size_t bytes;
} Slot;

static int s_init = 0, s_enabled = 0;
static Movie s_movies[FMV_MAX_MOVIES];
static int s_nmovies = 0;
static Slot s_ring[FMV_RING];
static SDL_mutex *s_mutex;
static SDL_cond *s_cond;
static SDL_Thread *s_workers[FMV_WORKERS];
static int s_stopping;
static size_t s_decoded_bytes;
static size_t s_index_bytes;
static uint64_t s_clock = 0;
static int s_cur_movie = -1, s_cur_src = -1;
static uint64_t s_cur_since = 0;          /* performance counter at the last frame change */
static uint64_t s_hd_frames = 0, s_queries = 0, s_misses = 0;
static uint8_t *s_shown = NULL;           /* copy of the presented HD frame */
static size_t s_shown_cap = 0;
static int s_shown_w = 0, s_shown_h = 0;
static uint64_t s_shown_at = 0, s_held = 0; /* time of the last HD frame; frames held */
static int s_shown_movie = -1, s_shown_frame = -1;
static uint64_t s_shown_serial = 0;

/* Mid-movie, falling back to the original frame (4:3 framing, another size) for a single
 * frame makes the screen jump: the last HD frame is held while it is recent. */
static int hold_last(FmvHdImage *out) {
    if (!s_shown || !s_shown_w || !s_shown_at) return 0;
    const double age = (double)(SDL_GetPerformanceCounter() - s_shown_at) / (double)SDL_GetPerformanceFrequency();
    if (age > FMV_HOLD_S) return 0;
    out->rgba = s_shown; out->w = s_shown_w; out->h = s_shown_h; out->serial = s_shown_serial;
    ++s_held;
    return 1;
}

static int read_file(const char *path, uint8_t **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (unsigned long)n > FMV_FILE_MAX) { fclose(f); return 0; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(f); return 0; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out = b; *len = (size_t)n;
    return 1;
}

static void frame_path(const Movie *m, int frame, char *out, size_t cap) {
    snprintf(out, cap, "%s/frames/%06d.jpg", m->dir, frame);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return; }
    snprintf(out, cap, "%s/frames/%06d.png", m->dir, frame);
}

static int worker(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(s_mutex);
        Slot *job = NULL;
        while (!job) {
            if (s_stopping) { SDL_UnlockMutex(s_mutex); return 0; }
            for (int i = 0; i < FMV_RING; ++i)
                if (s_ring[i].state == 1) { job = &s_ring[i]; break; }
            if (!job) SDL_CondWait(s_cond, s_mutex);
        }
        job->state = 4;                    /* in progress */
        const int movie = job->movie, frame = job->frame;
        SDL_UnlockMutex(s_mutex);

        char path[1100];
        frame_path(&s_movies[movie], frame, path, sizeof path);
        uint8_t *file = NULL; size_t len = 0;
        int w = 0, h = 0, n = 0;
        uint8_t *rgba = NULL;
        size_t reserved = 0;
        if (read_file(path, &file, &len)) {
            if (stbi_info_from_memory(file, (int)len, &w, &h, &n) &&
                w > 0 && h > 0 && w <= 8192 && h <= 8192 &&
                (uint64_t)w * h * 4 <= FMV_DECODE_MAX) {
                const size_t need = (size_t)w * h * 4;
                SDL_LockMutex(s_mutex);
                if (!s_stopping && need <= FMV_DECODE_MAX - s_decoded_bytes) {
                    s_decoded_bytes += need;
                    reserved = need;
                }
                SDL_UnlockMutex(s_mutex);
                if (reserved) {
                    int actual_w, actual_h;
                    rgba = stbi_load_from_memory(file, (int)len, &actual_w, &actual_h, &n, 4);
                    if (rgba && (actual_w != w || actual_h != h)) {
                        stbi_image_free(rgba); rgba = NULL;
                    }
                }
            }
            free(file);
        }
        SDL_LockMutex(s_mutex);
        if (!s_stopping && job->movie == movie && job->frame == frame && job->state == 4) {
            job->rgba = rgba; job->w = w; job->h = h;
            job->bytes = rgba ? reserved : 0;
            job->state = rgba ? 2 : 3;
        } else if (rgba) {
            stbi_image_free(rgba);
            rgba = NULL;
        }
        if (!rgba) s_decoded_bytes -= reserved;
        SDL_UnlockMutex(s_mutex);
    }
    return 0;
}

static void load_movie(const char *dir) {
    if (s_nmovies >= FMV_MAX_MOVIES) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/index.bin", dir);
    uint8_t *b = NULL; size_t len = 0;
    if (!read_file(path, &b, &len)) return;
    FmvHdIndexHeader h;
    if (len < sizeof h) { free(b); return; }
    memcpy(&h, b, sizeof h);
    const uint64_t sig = (uint64_t)h.sig_w * h.sig_h;
    const uint64_t bytes = sig * h.frames;
    if (memcmp(h.magic, "FMVHD002", 8) != 0 || !h.frames || !h.sub ||
        !h.sig_w || h.sig_w > 64 || !h.sig_h || h.sig_h > 64 ||
        h.sub > INT_MAX / 2 || (uint64_t)h.frames * h.sub > INT_MAX - FMV_RING ||
        h.src_w > 8192 || h.src_h > 8192 || h.disp_w > 8192 || h.disp_h > 8192 ||
        h.off_x < -1 || h.off_x > 8192 || h.off_y < -1 || h.off_y > 8192 ||
        bytes > FMV_FILE_MAX - sizeof h || bytes > len - sizeof h ||
        bytes > FMV_FILE_MAX - s_index_bytes) { free(b); return; }
    Movie *m = &s_movies[s_nmovies];
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    m->h = h;
    m->sigs = (uint8_t *)malloc((size_t)bytes);
    if (!m->sigs) { free(b); return; }
    memcpy(m->sigs, b + sizeof h, (size_t)bytes);
    s_index_bytes += (size_t)bytes;
    free(b);
    ++s_nmovies;
}

static void init_once(void) {
    if (s_init) return;
    s_init = 1;
    char root[1024];
    const char *env = getenv("PSX_FMV_HD_DIR");
    if (env && env[0] == '0' && env[1] == 0) return;
    if (env && env[0]) snprintf(root, sizeof root, "%s", env);
    else {
        const char *base = SDL_GetBasePath();
        snprintf(root, sizeof root, "%sfmv_hd", base ? base : "");
#if !defined(PSX_SDL3)
        SDL_free((void *)base);
#endif
    }
    /* Movie folders: liste.txt (one name per line), portable without dirent. */
    char list[1100];
    snprintf(list, sizeof list, "%s/liste.txt", root);
    uint8_t *b = NULL; size_t len = 0;
    if (!read_file(list, &b, &len)) {
        return;
    }
    char *txt = (char *)realloc(b, len + 1);
    if (!txt) { free(b); return; }
    txt[len] = 0;
    for (char *line = strtok(txt, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!line[0] || line[0] == '#') continue;
        if (strchr(line, '/') || strchr(line, '\\') || strchr(line, ':') ||
            !strcmp(line, ".") || !strcmp(line, "..")) continue;
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/%s", root, line);
        load_movie(dir);
    }
    free(txt);
    if (!s_nmovies) return;
    s_mutex = SDL_CreateMutex();
    s_cond = SDL_CreateCond();
    if (!s_mutex || !s_cond) { gpu_fmv_hd_shutdown(); s_init = 1; return; }
    for (int i = 0; i < FMV_RING; ++i) { s_ring[i].movie = -1; s_ring[i].frame = -1; }
    for (int i = 0; i < FMV_WORKERS; ++i) {
#ifdef PSX_FMV_HD_TESTING
        s_workers[i] = fmv_hd_test_start_thread(worker, NULL);
#else
        s_workers[i] = SDL_CreateThread(worker, "psx-fmv-hd", NULL);
#endif
        if (!s_workers[i]) { gpu_fmv_hd_shutdown(); s_init = 1; return; }
    }
    s_enabled = 1;
}

void gpu_fmv_hd_shutdown(void) {
    s_enabled = 0;
    if (s_mutex) {
        SDL_LockMutex(s_mutex);
        s_stopping = 1;
        if (s_cond) SDL_CondBroadcast(s_cond);
        SDL_UnlockMutex(s_mutex);
    }
    for (int i = 0; i < FMV_WORKERS; ++i) if (s_workers[i]) {
        SDL_WaitThread(s_workers[i], NULL); s_workers[i] = NULL;
    }
    for (int i = 0; i < FMV_RING; ++i) {
        if (s_ring[i].rgba) stbi_image_free(s_ring[i].rgba);
    }
    for (int i = 0; i < s_nmovies; ++i) free(s_movies[i].sigs);
    memset(s_ring, 0, sizeof s_ring);
    memset(s_movies, 0, sizeof s_movies);
    if (s_cond) SDL_DestroyCond(s_cond);
    if (s_mutex) SDL_DestroyMutex(s_mutex);
    s_cond = NULL; s_mutex = NULL;
    free(s_shown); s_shown = NULL;
    s_shown_cap = s_decoded_bytes = s_index_bytes = 0;
    s_shown_w = s_shown_h = s_nmovies = s_init = s_stopping = 0;
    s_cur_movie = s_cur_src = s_shown_movie = s_shown_frame = -1;
    s_cur_since = s_shown_at = s_shown_serial = s_clock = 0;
}

static void signature(const uint32_t *bgra, int src_w, int x0, int y0, int w, int h,
                      int sw, int sh, uint8_t *out) {
    for (int gy = 0; gy < sh; ++gy) {
        const int ya = y0 + gy * h / sh, yb = y0 + (gy + 1) * h / sh;
        for (int gx = 0; gx < sw; ++gx) {
            const int xa = x0 + gx * w / sw, xb = x0 + (gx + 1) * w / sw;
            uint64_t sum = 0;
            uint32_t n = 0;
            for (int y = ya; y < yb; ++y)
                for (int x = xa; x < xb; ++x) {
                    const uint32_t p = bgra[(size_t)y * src_w + x];
                    const uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                    sum += (r * 77 + g * 150 + b * 29) >> 8;
                    ++n;
                }
            out[gy * sw + gx] = (uint8_t)(n ? sum / n : 0);
        }
    }
}

static unsigned distance(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned d = 0;
    for (size_t i = 0; i < n; ++i) d += (unsigned)abs((int)a[i] - (int)b[i]);
    return (unsigned)(d / n);
}

static Slot *slot_for(int movie, int frame, int request) {
    for (int i = 0; i < FMV_RING; ++i)
        if (s_ring[i].movie == movie && s_ring[i].frame == frame) { s_ring[i].used = ++s_clock; return &s_ring[i]; }
    if (!request) return NULL;
    int victim = -1;
    for (int i = 0; i < FMV_RING; ++i) {
        if (s_ring[i].state == 4 || s_ring[i].state == 1) continue;
        if (victim < 0 || s_ring[i].used < s_ring[victim].used) victim = i;
    }
    if (victim < 0) return NULL;
    Slot *s = &s_ring[victim];
    if (s->rgba) { stbi_image_free(s->rgba); s->rgba = NULL; }
    s_decoded_bytes -= s->bytes; s->bytes = 0;
    s->movie = movie; s->frame = frame; s->state = 1; s->used = ++s_clock;
    SDL_CondSignal(s_cond);
    return s;
}

static unsigned s_dbg_best = ~0u;
static int s_dbg_src_w, s_dbg_src_h, s_dbg_frame = -1;

int gpu_fmv_hd_lookup(const uint32_t *bgra, int src_w, int src_h, int content_w, FmvHdImage *out) {
    init_once();
    if (!s_enabled || !bgra || src_w <= 0 || src_h <= 0 ||
        src_w > 16384 || src_h > 16384 || !out) return 0;
    ++s_queries;
    s_dbg_src_w = src_w; s_dbg_src_h = src_h; s_dbg_best = ~0u;
    const int cw = (content_w > 0 && content_w < src_w) ? content_w : src_w;

    /* Local search, then global. In a global search a nearly recognised movie is also tried
     * shifted by a few lines (each movie has its own position on screen); the offset found
     * is kept for the frames that follow. */
    static const int k_dys[] = { 0, -8, 8, -16, 16, -4, 4 };
    int best_m = -1, best_f = -1; unsigned best_d = ~0u;
    uint8_t sig[64 * 64];
    for (int pass = 0; pass < 2 && best_m < 0; ++pass) {
        for (int m = 0; m < s_nmovies; ++m) {
            Movie *mv = &s_movies[m];
            const size_t n = (size_t)mv->h.sig_w * mv->h.sig_h;
            if (n > sizeof sig) continue;
            int lo = 0, hi = (int)mv->h.frames - 1;
            if (pass == 0) {
                if (m != s_cur_movie || s_cur_src < 0) continue;
                lo = s_cur_src - FMV_LOCAL_BACK; hi = s_cur_src + FMV_LOCAL_AHEAD;
                if (lo < 0) lo = 0;
                if (hi > (int)mv->h.frames - 1) hi = (int)mv->h.frames - 1;
            }
            const int ntry = pass == 0 ? 1 : (int)(sizeof k_dys / sizeof k_dys[0]);
            unsigned m_d = ~0u; int m_f = -1, m_dy = mv->dy;
            for (int t = 0; t < ntry; ++t) {
                if (t == 1 && m_d > FMV_NEAR_GLOBAL) break;
                if (t > 0 && m_d <= FMV_ACCEPT_GLOBAL / 2) break;
                const int dy = mv->dy + k_dys[t];
                /* A src_w x src_h movie placed at (off_x, off_y) of a native disp_w x disp_h
                 * display, presented here scaled to cw x src_h. */
                const int dw = mv->h.disp_w ? (int)mv->h.disp_w : cw;
                const int dh = mv->h.disp_h ? (int)mv->h.disp_h : src_h;
                int fw = (int)mv->h.src_w * cw / dw, fh = (int)mv->h.src_h * src_h / dh;
                if (fw <= 0 || fw > cw) fw = cw;
                if (fh <= 0 || fh > src_h) fh = src_h;
                const int oy = (mv->h.off_y < 0 ? ((int)dh - (int)mv->h.src_h) / 2 : mv->h.off_y) + dy;
                int x0 = mv->h.off_x < 0 ? (cw - fw) / 2 : mv->h.off_x * cw / dw;
                int y0 = oy * src_h / dh;
                if (x0 + fw > cw) x0 = cw - fw;
                if (y0 + fh > src_h || y0 < 0) continue;
                signature(bgra, src_w, x0, y0, fw, fh, (int)mv->h.sig_w, (int)mv->h.sig_h, sig);
                for (int f = lo; f <= hi; ++f) {
                    const unsigned d = distance(sig, mv->sigs + (size_t)f * n, n);
                    if (d < m_d) { m_d = d; m_f = f; m_dy = dy; }
                }
            }
            if (m_d < s_dbg_best) { s_dbg_best = m_d; s_dbg_frame = m_f; }
            if (m_d <= (unsigned)(pass == 0 ? FMV_ACCEPT_LOCAL : FMV_ACCEPT_GLOBAL) && m_d < best_d) {
                best_d = m_d; best_m = m; best_f = m_f; mv->dy = m_dy;
            }
        }
    }
    if (best_m < 0) { ++s_misses; s_cur_movie = -1; s_cur_src = -1; return hold_last(out); }

    const uint64_t now = SDL_GetPerformanceCounter();
    if (best_m != s_cur_movie || best_f != s_cur_src) {
        s_cur_movie = best_m; s_cur_src = best_f; s_cur_since = now;
    }
    const Movie *mv = &s_movies[best_m];
    int k = 0;
    if (mv->h.sub > 1 && mv->h.fps_num) {
        const double t = (double)(now - s_cur_since) / (double)SDL_GetPerformanceFrequency();
        const double phase = t * mv->h.fps_num / (mv->h.fps_den ? mv->h.fps_den : 1) * mv->h.sub;
        if (phase >= mv->h.sub) k = (int)mv->h.sub - 1;
        else if (phase > 0) k = (int)phase;
    }
    const int hd = best_f * (int)mv->h.sub + k;
    const int last = (int)(mv->h.frames * mv->h.sub) - 1;

    SDL_LockMutex(s_mutex);
    /* Request the next frames ahead. */
    for (int a = 0; a < FMV_RING - 4 && hd + a <= last; ++a) slot_for(best_m, hd + a, 1);
    Slot *s = slot_for(best_m, hd, 0);
    /* Exact frame not ready yet: the previous decoded one, else the original. */
    for (int back = 0; (!s || s->state != 2) && back < (int)mv->h.sub * 2 && hd - back > 0; ++back)
        s = slot_for(best_m, hd - back - 1, 0);
    int ok = 0;
    if (s && s->state == 2 && s->rgba) {
        const size_t need = (size_t)s->w * s->h * 4;
        if (s->movie == s_shown_movie && s->frame == s_shown_frame && s_shown) {
            ok = 1;            /* same frame as the previous refresh: no copy */
        } else {
            if (need > s_shown_cap) {
                uint8_t *next = (uint8_t *)realloc(s_shown, need);
                if (next) { s_shown = next; s_shown_cap = need; }
                else { SDL_UnlockMutex(s_mutex); return 0; }
            }
            if (s_shown) {
                memcpy(s_shown, s->rgba, need);
                s_shown_w = s->w; s_shown_h = s->h;
                s_shown_movie = s->movie; s_shown_frame = s->frame; ++s_shown_serial;
                ok = 1;
            }
        }
        if (ok) {
            out->rgba = s_shown; out->w = s_shown_w; out->h = s_shown_h; out->serial = s_shown_serial;
            s_shown_at = now;
        }
    }
    SDL_UnlockMutex(s_mutex);
    if (ok) { ++s_hd_frames; return 1; }
    return hold_last(out);
}

void gpu_fmv_hd_stats(uint64_t *hd_frames, uint64_t *queries, uint64_t *misses) {
    if (hd_frames) *hd_frames = s_hd_frames;
    if (queries) *queries = s_queries;
    if (misses) *misses = s_misses;
}

int gpu_fmv_hd_debug_json(char *out, size_t cap) {
    return snprintf(out, cap,
        "\"init\":%d,\"enabled\":%d,\"movies\":%d,\"queries\":%llu,\"hd_frames\":%llu,"
        "\"misses\":%llu,\"last_src\":[%d,%d],\"last_best_dist\":%u,\"last_best_frame\":%d,"
        "\"cur_src\":%d,\"dy\":%d,\"held\":%llu",
        s_init, s_enabled, s_nmovies, (unsigned long long)s_queries, (unsigned long long)s_hd_frames,
        (unsigned long long)s_misses, s_dbg_src_w, s_dbg_src_h, s_dbg_best, s_dbg_frame, s_cur_src,
        s_cur_movie >= 0 ? s_movies[s_cur_movie].dy : 0, (unsigned long long)s_held);
}
