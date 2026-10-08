/* psx_mod_set_pad_transform registration rules and per-frame validation. */
#include "mod_pad_transform.h"
#include "sio.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static int mode, calls;
static PSXModPadFrame seen;

static int xf(const PSXModPadFrame *f, PSXModPadOutput *o) {
    calls++;
    seen = *f;
    if (mode == 1) return 0;                 /* decline: pass through */
    o->type = PSX_MOD_PAD_NEGCON;
    o->buttons = f->buttons | 0x0400u;       /* drop L1 */
    o->negcon_i = f->host_rt;
    o->negcon_ii = f->host_lt;
    if (mode == 2) o->negcon_i = 256;        /* out of range */
    if (mode == 3) o->type = PSX_MOD_PAD_JOGCON; /* not allowed */
    if (mode == 4) o->struct_size = 4;
    return 1;
}

static PSXModPadFrame frame(void) {
    PSXModPadFrame f;
    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.buttons = 0xfbffu;  /* L1 held */
    f.lx = 0x40; f.ly = 0x80; f.rx = 0x90; f.ry = 0x70;
    f.type = PSX_MOD_PAD_DUALSHOCK;
    f.host_flags = 7; f.host_lt = 30; f.host_rt = 200;
    return f;
}

int main(void) {
    PSXModPadTransform t;
    PSXModPadFrame f = frame();
    PSXModPadOutput o;
    memset(&t, 0, sizeof t);
    t.struct_size = sizeof t;
    t.allowed_types = PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DUALSHOCK) |
                      PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_NEGCON);
    t.initial_type = PSX_MOD_PAD_DUALSHOCK;
    t.transform = xf;

    /* Nothing registered: no stage, nothing owed. */
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 0);
    CHECK(mod_pad_transform_initial_type(0) == -1);

    /* Registration validation. */
    CHECK(!psx_mod_set_pad_transform(PSX_MAX_PLAYERS, &t));
    { PSXModPadTransform b = t; b.struct_size = 8; CHECK(!psx_mod_set_pad_transform(0, &b)); }
    { PSXModPadTransform b = t; b.transform = NULL; CHECK(!psx_mod_set_pad_transform(0, &b)); }
    { PSXModPadTransform b = t; b.allowed_types = 0; CHECK(!psx_mod_set_pad_transform(0, &b)); }
    { PSXModPadTransform b = t; b.allowed_types |= 1u << 4; CHECK(!psx_mod_set_pad_transform(0, &b)); }
    { PSXModPadTransform b = t; b.initial_type = PSX_MOD_PAD_JOGCON; CHECK(!psx_mod_set_pad_transform(0, &b)); }
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 0);
    CHECK(psx_mod_set_pad_transform(0, &t));
    CHECK(mod_pad_transform_initial_type(0) == PSX_MOD_PAD_DUALSHOCK);
    CHECK(mod_pad_transform_initial_type(1) == -1);

    /* Applied output; the frame carries pad + host extras. */
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 1);
    CHECK(seen.player == 0 && seen.host_rt == 200 && seen.type == PSX_MOD_PAD_DUALSHOCK);
    CHECK(o.type == PSX_MOD_PAD_NEGCON && o.buttons == 0xffffu &&
          o.negcon_i == 200 && o.negcon_ii == 30 && o.negcon_l == 0);
    CHECK(o.lx == 0x40 && o.rx == 0x90);  /* pre-filled pass-through */

    /* Decline -> exact pass-through. */
    mode = 1;
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 1);
    CHECK(o.type == PSX_MOD_PAD_DUALSHOCK && o.buttons == 0xfbffu &&
          o.lx == 0x40 && o.ry == 0x70 && o.negcon_i == 0);

    /* Decline with a stock pad -> that stock pad, not the frame (a digital
     * player's frame is its host pad; the stock pad keeps the fold). */
    {
        PSXModPadOutput stock;
        memset(&stock, 0, sizeof stock);
        stock.struct_size = sizeof stock;
        stock.buttons = 0xff7fu; stock.type = PSX_MOD_PAD_DIGITAL;
        stock.lx = stock.ly = stock.rx = stock.ry = 0x80;
        CHECK(mod_pad_transform_run(0, &f, &stock, &o) == 1);
        CHECK(memcmp(&o, &stock, sizeof o) == 0);
        mode = 0;   /* applied: pre-filled from the stock pad */
        CHECK(mod_pad_transform_run(0, &f, &stock, &o) == 1);
        CHECK(o.type == PSX_MOD_PAD_NEGCON && o.lx == 0x80 && o.negcon_i == 200);
        mode = 1;
    }

    /* Invalid output (range, disallowed type, struct size) -> neutral of the
     * frame's type. */
    for (mode = 2; mode <= 4; mode++) {
        CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 1);
        CHECK(o.type == PSX_MOD_PAD_DUALSHOCK && o.buttons == 0xffffu &&
              o.lx == 0x80 && o.ry == 0x80 && o.negcon_i == 0);
    }

    /* Detach: one neutral release frame, then nothing; the callback is not
     * called for the release. */
    mode = 0;
    CHECK(psx_mod_set_pad_transform(0, NULL));
    calls = 0;
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 1);
    CHECK(calls == 0 && o.buttons == 0xffffu && o.type == PSX_MOD_PAD_DUALSHOCK &&
          o.lx == 0x80);
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 0);
    CHECK(mod_pad_transform_initial_type(0) == -1);

    /* Session reset: same release for every registered player. */
    CHECK(psx_mod_set_pad_transform(0, &t) && psx_mod_set_pad_transform(1, &t));
    mod_pad_transform_reset();
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 1 && o.buttons == 0xffffu);
    CHECK(mod_pad_transform_run(1, &f, NULL, &o) == 1 && o.buttons == 0xffffu);
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 0 && mod_pad_transform_run(1, &f, NULL, &o) == 0);
    /* A reset with nothing registered owes nothing. */
    mod_pad_transform_reset();
    CHECK(mod_pad_transform_run(0, &f, NULL, &o) == 0);
    fprintf(stderr, "mod_pad_transform: passed\n");
    return 0;
}
