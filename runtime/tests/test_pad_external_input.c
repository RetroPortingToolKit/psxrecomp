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
    int xf_ret; PSXModPadOutput xf_out; PSXModPadFrame xf_seen; int xf_calls;
    PSXModPadOutput xf_stock; int xf_echo_stock;
    int host_pad_ok, host_pad_calls; uint16_t host_buttons; uint8_t host_st[4];
    uint32_t ex_flags, ex_lt, ex_rt;
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

static void h_extras(void *c, int s, uint32_t *f, uint32_t *lt, uint32_t *rt) {
    (void)c; (void)s; L('e'); *f = T.ex_flags; *lt = T.ex_lt; *rt = T.ex_rt;
}
static int h_xf(void *c, int s, const PSXModPadFrame *f,
                const PSXModPadOutput *stock, PSXModPadOutput *o) {
    (void)c; (void)s; L('t'); T.xf_calls++; T.xf_seen = *f; T.xf_stock = *stock;
    if (!T.xf_ret) return 0;
    *o = T.xf_echo_stock ? *stock : T.xf_out; return 1;
}
static int h_host_pad(void *c, int s, uint16_t *b, uint8_t st[4]) {
    (void)c; (void)s; L('h'); T.host_pad_calls++;
    if (!T.host_pad_ok) return 0;
    *b = T.host_buttons; memcpy(st, T.host_st, 4); return 1;
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

    /* Stage 2b, title transform. Not registered (hook returns 0): the pad of
     * stages 1-2 passes through bit-identical. */
    {
        PadExtHooks hx = hooks();
        hx.host_extras = h_extras; hx.pad_transform = h_xf;
        reset();
        CHECK(pad_ext_resolve(&hx, 0, &o) == 1);
        CHECK(o.buttons == 0xfffe && o.lx == 10 && o.ly == 20 && o.rx == 30 &&
              o.ry == 40 && o.analog == 1);
        CHECK(strcmp(T.log, "gscetm") == 0);

        /* It sees the resolved pad (after the source) plus host extras, and a
         * NeGcon output is packed lx twist, rx I, ry II, ly L; the mouse is
         * reset rather than folded into the pressures. */
        reset();
        T.have_source = 1;
        T.ex_flags = 7; T.ex_lt = 60; T.ex_rt = 250;
        T.xf_ret = 1;
        T.xf_out.struct_size = sizeof T.xf_out; T.xf_out.buttons = 0xbfff;
        T.xf_out.type = PSX_MOD_PAD_NEGCON; T.xf_out.lx = 0x30;
        T.xf_out.ly = 0x99; T.xf_out.rx = 0x98; T.xf_out.ry = 0x97;
        T.xf_out.negcon_i = 250; T.xf_out.negcon_ii = 60; T.xf_out.negcon_l = 5;
        T.mouse_override = 1; T.mouse_rx = 1; T.mouse_ry = 2;
        CHECK(pad_ext_resolve(&hx, 0, &o) == 1);
        CHECK(T.xf_seen.struct_size == sizeof T.xf_seen && T.xf_seen.player == 0);
        CHECK(T.xf_seen.buttons == (0xfeff & 0xfffe) && T.xf_seen.lx == 200 &&
              T.xf_seen.type == 1 && T.xf_seen.host_flags == 7 &&
              T.xf_seen.host_lt == 60 && T.xf_seen.host_rt == 250);
        CHECK(o.analog == PSX_MOD_PAD_NEGCON && o.buttons == 0xbfff &&
              o.lx == 0x30 && o.rx == 250 && o.ry == 60 && o.ly == 5);
        CHECK(T.mouse_folds == 0 && T.mouse_resets == 1);
        CHECK(strcmp(T.log, "gscretx") == 0);

        /* DualShock output: sticks from the output, mouse folds as usual. */
        reset();
        T.xf_ret = 1; T.xf_out.struct_size = sizeof T.xf_out;
        T.xf_out.buttons = 0xfff0; T.xf_out.type = PSX_MOD_PAD_DUALSHOCK;
        T.xf_out.lx = 1; T.xf_out.ly = 2; T.xf_out.rx = 3; T.xf_out.ry = 4;
        CHECK(pad_ext_resolve(&hx, 0, &o) == 1);
        CHECK(o.analog == 1 && o.lx == 1 && o.ly == 2 && o.rx == 3 && o.ry == 4 &&
              T.mouse_folds == 1 && T.fold_rx == 3 && T.fold_buttons == 0xfff0);

        /* Digital output centres the sticks. */
        T.xf_out.type = PSX_MOD_PAD_DIGITAL;
        CHECK(pad_ext_resolve(&hx, 1, &o) == 1 && o.analog == 0 && o.lx == 0x80);

        /* Input guard: the transform still runs, sees no trigger values, and
         * its output is neutralized while keeping the presented type. */
        reset(); T.guard = 1; T.ex_flags = 7; T.ex_lt = 99; T.ex_rt = 99;
        T.xf_ret = 1; T.xf_out.struct_size = sizeof T.xf_out;
        T.xf_out.buttons = 0x0000; T.xf_out.type = PSX_MOD_PAD_NEGCON;
        T.xf_out.negcon_i = 200;
        CHECK(pad_ext_resolve(&hx, 0, &o) == 1);
        CHECK(T.xf_seen.host_flags == 7 && T.xf_seen.host_lt == 0 && T.xf_seen.host_rt == 0);
        CHECK(o.analog == PSX_MOD_PAD_NEGCON && o.buttons == 0xffff &&
              o.lx == 0x80 && o.rx == 0 && o.ry == 0 && o.ly == 0);

        /* Digital-presented player: the frame is the host pad (real sticks,
         * buttons without the stick->D-pad fold, type still digital); the
         * stock output, delivered on decline, is the folded digital pad. */
        hx.host_pad = h_host_pad;
        {
            const uint16_t folded = 0xff7e;   /* SELECT + LEFT from the stick */
            reset();
            T.local.analog = 0; T.local.buttons = folded;
            T.local.lx = T.local.ly = T.local.rx = T.local.ry = 0x80;
            T.host_pad_ok = 1; T.host_buttons = 0xfffe;
            T.host_st[0] = 0x10; T.host_st[1] = 0x80; T.host_st[2] = 0x81; T.host_st[3] = 0x7f;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1);
            CHECK(T.host_pad_calls == 1 && T.xf_seen.type == PSX_MOD_PAD_DIGITAL &&
                  T.xf_seen.lx == 0x10 && T.xf_seen.rx == 0x81 &&
                  T.xf_seen.buttons == 0xfffe);
            CHECK(T.xf_stock.buttons == folded && T.xf_stock.lx == 0x80 &&
                  T.xf_stock.type == PSX_MOD_PAD_DIGITAL);
            CHECK(o.buttons == folded && o.analog == 0 && o.lx == 0x80 &&
                  o.ly == 0x80 && o.rx == 0x80 && o.ry == 0x80 && o.connected == 1);
            /* Declined through mod_pad_transform_run (stock echoed back):
             * still the stock digital bytes. */
            reset();
            T.local.analog = 0; T.local.buttons = folded;
            T.local.lx = T.local.ly = T.local.rx = T.local.ry = 0x80;
            T.host_pad_ok = 1; T.host_buttons = 0xfffe; T.host_st[0] = 0x10;
            T.xf_ret = 1; T.xf_echo_stock = 1;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1);
            CHECK(o.buttons == folded && o.analog == 0 && o.lx == 0x80 &&
                  o.ly == 0x80 && o.rx == 0x80 && o.ry == 0x80);
            /* A NeGcon from the real stick. */
            T.xf_echo_stock = 0; T.xf_out.struct_size = sizeof T.xf_out;
            T.xf_out.type = PSX_MOD_PAD_NEGCON; T.xf_out.buttons = 0xffff;
            T.xf_out.lx = 0x10;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1 && o.analog == PSX_MOD_PAD_NEGCON &&
                  o.lx == 0x10);
            /* Analog pads, guarded frames and source-driven ports keep the
             * resolved pad as the frame; no host-pad query. */
            reset(); T.host_pad_ok = 1; T.host_st[0] = 0x10;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1 && T.host_pad_calls == 0 &&
                  T.xf_seen.lx == 10);
            reset(); T.local.analog = 0; T.guard = 1; T.host_pad_ok = 1;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1 && T.host_pad_calls == 0);
            reset(); T.have_source = 1; T.src_analog_out = 0; T.host_pad_ok = 1;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1 && T.host_pad_calls == 0);
            /* Unavailable host pad: the frame stays the stock pad. */
            reset(); T.local.analog = 0; T.local.buttons = folded;
            T.local.lx = 0x80;
            CHECK(pad_ext_resolve(&hx, 0, &o) == 1 && T.host_pad_calls == 1 &&
                  T.xf_seen.buttons == folded && T.xf_seen.lx == 0x80);
        }

        /* No device and no source: the transform is not consulted. */
        reset(); T.have_device = 0; T.xf_ret = 1;
        CHECK(pad_ext_resolve(&hx, 0, &o) == 0 && T.xf_calls == 0);
    }

    /* Gating predicate: live only when every blocking condition is clear. */
    {
        PadExtGate g; memset(&g, 0, sizeof g);
        CHECK(pad_ext_live(&g) == 1 && pad_ext_live(NULL) == 0);
        int *f[] = { &g.injected_input, &g.headless, &g.netplay_active,
                     &g.netplay_resim, &g.selfcheck_locked, &g.selfcheck_resim,
                     &g.render_pass, &g.savestate_menu_open, &g.rewind_open,
                     &g.input_guard, &g.ui_capture };
        for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
            *f[i] = 1; CHECK(pad_ext_live(&g) == 0); *f[i] = 0;
        }
        CHECK(pad_ext_live(&g) == 1);
    }
    return 0;
}
