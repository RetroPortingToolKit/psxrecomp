#include <string.h>
#include "pad_external_input.h"

int pad_ext_live(const PadExtGate *g) {
    return g && !g->injected_input && !g->headless && !g->netplay_active &&
           !g->netplay_resim && !g->selfcheck_locked && !g->selfcheck_resim &&
           !g->render_pass && !g->savestate_menu_open && !g->rewind_open &&
           !g->input_guard && !g->ui_capture;
}

/* Stage 2b: run the title transform over the resolved pad and pack its
 * output back into the PsxNetPad (NeGcon: lx twist, rx I, ry II, ly L). */
static void pad_ext_transform(const PadExtHooks *h, int s, int guarded,
                              int from_source, PsxNetPad *out) {
    PSXModPadFrame f;
    PSXModPadOutput stock, o;
    if (!h->pad_transform) return;
    memset(&stock, 0, sizeof stock);
    stock.struct_size = sizeof stock;
    stock.buttons = out->buttons;
    stock.type = out->analog;
    stock.lx = out->lx; stock.ly = out->ly; stock.rx = out->rx; stock.ry = out->ry;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.player = (uint32_t)s;
    f.buttons = out->buttons;
    f.lx = out->lx; f.ly = out->ly; f.rx = out->rx; f.ry = out->ry;
    f.type = out->analog;
    /* A digital presentation folds the sticks onto the D-pad and centres
     * them; the transform decides what the guest sees, so its frame is the
     * host pad instead. The stock (pass-through) output keeps the fold. */
    if (out->analog == PSX_MOD_PAD_DIGITAL && !from_source && !guarded &&
        h->host_pad) {
        uint16_t hb;
        uint8_t st[4];
        if (h->host_pad(h->ctx, s, &hb, st)) {
            f.buttons = hb;
            f.lx = st[0]; f.ly = st[1]; f.rx = st[2]; f.ry = st[3];
        }
    }
    if (h->host_extras) h->host_extras(h->ctx, s, &f.host_flags, &f.host_lt, &f.host_rt);
    if (guarded) f.host_lt = f.host_rt = 0;
    if (!h->pad_transform(h->ctx, s, &f, &stock, &o)) return;
    if (guarded) {
        o.buttons = 0xffff;
        o.lx = o.ly = o.rx = o.ry = 0x80;
        o.negcon_i = o.negcon_ii = o.negcon_l = 0;
    }
    out->buttons = (uint16_t)o.buttons;
    out->analog = (uint8_t)o.type;
    if (o.type == PSX_MOD_PAD_NEGCON) {
        out->lx = (uint8_t)o.lx;
        out->ly = (uint8_t)o.negcon_l;
        out->rx = (uint8_t)o.negcon_i;
        out->ry = (uint8_t)o.negcon_ii;
    } else if (o.type == PSX_MOD_PAD_DIGITAL) {
        out->lx = out->ly = out->rx = out->ry = 0x80;
    } else {
        out->lx = (uint8_t)o.lx; out->ly = (uint8_t)o.ly;
        out->rx = (uint8_t)o.rx; out->ry = (uint8_t)o.ry;
    }
}

int pad_ext_resolve(const PadExtHooks *h, int s, PsxNetPad *out) {
    PSXModControllerState src;
    int released = 0;
    const int guarded = h->guard_active ? h->guard_active(h->ctx) : 0;
    const int has_source = h->source_sample &&
        h->source_sample(h->ctx, s, &src, &released);

    if (has_source && released) {
        h->source_release(h->ctx, s, out);
        if (s == 0) h->mouse_reset(h->ctx);
        return 1;
    }
    if (has_source) {
        PsxNetPad local;
        const int have_local = h->capture_local(h->ctx, s, &local, guarded);
        out->buttons = (uint16_t)src.buttons;
        if (have_local) out->buttons &= local.buttons;
        h->source_resolve(h->ctx, s, &src, guarded, out);
    } else if (!h->capture_local(h->ctx, s, out, guarded)) {
        if (s == 0) h->mouse_reset(h->ctx);
        return 0;
    }
    pad_ext_transform(h, s, guarded, has_source, out);
    if (s == 0) {
        if (guarded || out->analog == PSX_MOD_PAD_NEGCON) h->mouse_reset(h->ctx);
        else h->mouse_fold(h->ctx, out->connected, out->analog, out->buttons,
                           &out->rx, &out->ry);
    }
    return 1;
}
