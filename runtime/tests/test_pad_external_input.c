/* Resolution order of external offline input (physical -> source -> mouse). */
#include "pad_external_input.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static struct {
    int guard, have_device, have_source, released;
    PsxNetPad local; PSXModControllerState src;
    int mouse_override; uint8_t mouse_rx, mouse_ry;
    char log[64]; int n;
    int capture_calls, source_calls, resolve_calls, release_calls;
    int mouse_resets, mouse_folds, last_guarded_cap, last_guarded_res;
    int fold_connected, fold_analog; uint16_t fold_buttons; uint8_t fold_rx, fold_ry;
    int fold_slot_calls[2];
    int src_analog_out;
} T;

static void L(char c) { T.log[T.n++] = c; T.log[T.n] = 0; }
static int  h_guard(void *c) { (void)c; L('g'); return T.guard; }
static int  h_cap(void *c, int s, PsxNetPad *o, int g) {
    (void)c; (void)s; L('c'); T.capture_calls++; T.last_guarded_cap = g;
    if (!T.have_device) { memset(o, 0, sizeof *o); return 0; }
    *o = T.local; if (g) { o->buttons = 0xffff; o->rx = o->ry = 0x80; }
    return 1;
}
static int  h_src(void *c, int s, PSXModControllerState *st, int *rel) {
    (void)c; (void)s; L('s'); T.source_calls++;
    if (!T.have_source) return 0;
    *st = T.src; *rel = T.released; return 1;
}
static void h_rel(void *c, int s, PsxNetPad *o) {
    (void)c; (void)s; L('R'); T.release_calls++;
    o->buttons = 0xffff; o->lx = o->ly = o->rx = o->ry = 128; o->analog = 1; o->connected = 1;
}
static void h_res(void *c, int s, const PSXModControllerState *st, int g, PsxNetPad *o) {
    (void)c; (void)s; L('r'); T.resolve_calls++; T.last_guarded_res = g;
    o->lx = (uint8_t)st->lx; o->ly = (uint8_t)st->ly;
    o->rx = (uint8_t)st->rx; o->ry = (uint8_t)st->ry;
    o->analog = (uint8_t)T.src_analog_out; o->connected = 1;
    if (!o->analog) o->lx = o->ly = o->rx = o->ry = 128;
    if (g) { o->buttons = 0xffff; o->lx = o->ly = o->rx = o->ry = 128; }
}
static void h_mreset(void *c) { (void)c; L('x'); T.mouse_resets++; }
static void h_mfold(void *c, int con, int an, uint16_t b, uint8_t *rx, uint8_t *ry) {
    (void)c; L('m'); T.mouse_folds++;
    T.fold_connected = con; T.fold_analog = an; T.fold_buttons = b;
    T.fold_rx = *rx; T.fold_ry = *ry;
    if (T.mouse_override) { *rx = T.mouse_rx; *ry = T.mouse_ry; }
}

static PadExtHooks hooks(void) {
    PadExtHooks h; memset(&h, 0, sizeof h);
    h.guard_active = h_guard; h.capture_local = h_cap; h.source_sample = h_src;
    h.source_release = h_rel; h.source_resolve = h_res;
    h.mouse_reset = h_mreset; h.mouse_fold = h_mfold;
    return h;
}
static void reset(void) {
    memset(&T, 0, sizeof T);
    T.have_device = 1; T.src_analog_out = 1;
    T.local.buttons = 0xfffe; /* SELECT held */
    T.local.lx = 10; T.local.ly = 20; T.local.rx = 30; T.local.ry = 40;
    T.local.analog = 1; T.local.connected = 1;
    T.src.struct_size = sizeof T.src; T.src.buttons = 0xfeff;
    T.src.lx = 200; T.src.ly = 5; T.src.rx = 128; T.src.ry = 128; T.src.analog = 1;
}

int main(void) {
    PadExtHooks h = hooks();
    PsxNetPad o;

    /* No source, no mouse effect: physical pad passes through bit-identical. */
    reset();
    CHECK(pad_ext_resolve(&h, 0, &o) == 1);
    CHECK(o.buttons == 0xfffe && o.lx == 10 && o.ly == 20 && o.rx == 30 &&
          o.ry == 40 && o.analog == 1 && o.connected == 1);
    CHECK(T.capture_calls == 1 && T.resolve_calls == 0 && T.release_calls == 0);
    CHECK(T.mouse_folds == 1 && T.fold_connected == 1 && T.fold_analog == 1 &&
          T.fold_buttons == 0xfffe && T.fold_rx == 30 && T.fold_ry == 40);
    CHECK(strcmp(T.log, "gscm") == 0);

    /* Other ports never reach the mouse; no device: 0 and P1 resets it. */
    reset();
    CHECK(pad_ext_resolve(&h, 1, &o) == 1 && T.mouse_folds == 0 && T.mouse_resets == 0);
    reset(); T.have_device = 0;
    CHECK(pad_ext_resolve(&h, 1, &o) == 0 && T.mouse_resets == 0);
    reset(); T.have_device = 0;
    CHECK(pad_ext_resolve(&h, 0, &o) == 0 && T.mouse_resets == 1 && T.mouse_folds == 0);

    /* Mouse only: overrides the right stick, left stick/buttons untouched. */
    reset(); T.local.rx = T.local.ry = 128; T.mouse_override = 1; T.mouse_rx = 250; T.mouse_ry = 7;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1);
    CHECK(o.rx == 250 && o.ry == 7 && o.lx == 10 && o.ly == 20 && o.buttons == 0xfffe);

    /* Source only: buttons ANDed with physical, sticks/type from the source,
     * physical capture sampled exactly once, mouse offered the RESOLVED pad. */
    reset();
    T.have_source = 1; T.src.rx = 90; T.src.ry = 100;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1);
    CHECK(o.buttons == (0xfeff & 0xfffe) && o.lx == 200 && o.ly == 5 &&
          o.rx == 90 && o.ry == 100 && o.analog == 1 && o.connected == 1);
    CHECK(T.capture_calls == 1 && T.resolve_calls == 1);
    CHECK(T.fold_buttons == (0xfeff & 0xfffe) && T.fold_analog == 1 &&
          T.fold_rx == 90 && T.fold_ry == 100); /* not the physical 30/40 */
    CHECK(strcmp(T.log, "gscrm") == 0);

    /* Source + mouse together: mouse overrides ONLY the right axes. */
    reset();
    T.have_source = 1; T.mouse_override = 1; T.mouse_rx = 33; T.mouse_ry = 44;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1);
    CHECK(o.rx == 33 && o.ry == 44 && o.lx == 200 && o.ly == 5 &&
          o.buttons == (0xfeff & 0xfffe) && T.mouse_resets == 0);

    /* Source on a port with no physical device: source buttons as-is. */
    reset(); T.have_source = 1; T.have_device = 0;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1 && o.buttons == 0xfeff && o.connected == 1);
    CHECK(T.mouse_folds == 1 && T.mouse_resets == 0);

    /* Digital source: sticks neutral, mouse sees analog=0. */
    reset(); T.have_source = 1; T.src_analog_out = 0;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1 && o.analog == 0 && o.lx == 128 &&
          T.fold_analog == 0);

    /* Source release frame: neutral pad, mouse released, no physical capture. */
    reset(); T.have_source = 1; T.released = 1;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1 && o.buttons == 0xffff && o.lx == 128);
    CHECK(T.release_calls == 1 && T.capture_calls == 0 && T.resolve_calls == 0);
    CHECK(T.mouse_resets == 1 && T.mouse_folds == 0);
    reset(); T.have_source = 1; T.released = 1;
    CHECK(pad_ext_resolve(&h, 1, &o) == 1 && T.mouse_resets == 0);

    /* Input guard: external input is neutral and the mouse is released; the
     * guard is forwarded to capture (physical) and resolve (source). */
    reset(); T.guard = 1;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1 && T.last_guarded_cap == 1);
    CHECK(o.buttons == 0xffff && o.rx == 128 && T.mouse_resets == 1 && T.mouse_folds == 0);
    reset(); T.guard = 1; T.have_source = 1;
    CHECK(pad_ext_resolve(&h, 0, &o) == 1 && T.last_guarded_res == 1);
    CHECK(o.buttons == 0xffff && o.lx == 128 && T.mouse_resets == 1 && T.mouse_folds == 0);

    /* Gating predicate: live only when every blocking condition is clear. */
    {
        PadExtGate g; memset(&g, 0, sizeof g);
        CHECK(pad_ext_live(&g) == 1 && pad_ext_live(NULL) == 0);
        int *f[] = { &g.injected_input, &g.headless, &g.netplay_active,
                     &g.netplay_resim, &g.selfcheck_locked, &g.selfcheck_resim,
                     &g.render_pass, &g.savestate_menu_open, &g.rewind_open,
                     &g.input_guard };
        for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
            *f[i] = 1; CHECK(pad_ext_live(&g) == 0); *f[i] = 0;
        }
        CHECK(pad_ext_live(&g) == 1);
    }
    return 0;
}
