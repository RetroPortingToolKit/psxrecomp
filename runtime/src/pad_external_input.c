#include "pad_external_input.h"

int pad_ext_live(const PadExtGate *g) {
    return g && !g->injected_input && !g->headless && !g->netplay_active &&
           !g->netplay_resim && !g->selfcheck_locked && !g->selfcheck_resim &&
           !g->render_pass && !g->savestate_menu_open && !g->rewind_open &&
           !g->input_guard;
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
    if (s == 0) {
        if (guarded) h->mouse_reset(h->ctx);
        else h->mouse_fold(h->ctx, out->connected, out->analog, out->buttons,
                           &out->rx, &out->ry);
    }
    return 1;
}
