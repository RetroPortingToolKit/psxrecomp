#include "mod_pad_transform.h"
#include "sio.h"
#include <stdio.h>
#include <string.h>

#define PAD_TYPE_ALL (PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DIGITAL) |   \
                      PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DUALSHOCK) | \
                      PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_JOGCON) |    \
                      PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_NEGCON))
#define BAD_LOG_INTERVAL 600u

static PSXModPadTransform s_xf[PSX_MAX_PLAYERS];
static uint8_t s_active[PSX_MAX_PLAYERS];
static uint8_t s_release[PSX_MAX_PLAYERS];
static uint32_t s_bad_count[PSX_MAX_PLAYERS];

int psx_mod_set_pad_transform(uint32_t player, const PSXModPadTransform *xf) {
    if (player >= PSX_MAX_PLAYERS) return 0;
    if (!xf) {
        if (s_active[player]) s_release[player] = 1;
        s_active[player] = 0;
        memset(&s_xf[player], 0, sizeof s_xf[player]);
        return 1;
    }
    if (xf->struct_size != sizeof *xf || !xf->transform ||
        !xf->allowed_types || (xf->allowed_types & ~PAD_TYPE_ALL) ||
        xf->initial_type > PSX_MOD_PAD_NEGCON ||
        !(xf->allowed_types & PSX_MOD_PAD_TYPE_BIT(xf->initial_type))) {
        fprintf(stderr,
            "psxrecomp: mod pad transform (player %u) rejected: struct_size %u "
            "(expected %u), callback %s, allowed_types 0x%x, initial_type %u\n",
            (unsigned)player, (unsigned)xf->struct_size, (unsigned)sizeof *xf,
            xf->transform ? "set" : "NULL", (unsigned)xf->allowed_types,
            (unsigned)xf->initial_type);
        return 0;
    }
    s_xf[player] = *xf;
    s_active[player] = 1;
    s_release[player] = 0;
    s_bad_count[player] = 0;
    return 1;
}

void mod_pad_transform_reset(void) {
    for (uint32_t i = 0; i < PSX_MAX_PLAYERS; i++) {
        if (s_active[i]) s_release[i] = 1;
        s_active[i] = 0;
    }
    memset(s_xf, 0, sizeof s_xf);
    memset(s_bad_count, 0, sizeof s_bad_count);
}

int mod_pad_transform_initial_type(uint32_t player) {
    if (player >= PSX_MAX_PLAYERS || !s_active[player]) return -1;
    return (int)s_xf[player].initial_type;
}

static void pass_through(const PSXModPadFrame *f, PSXModPadOutput *o) {
    memset(o, 0, sizeof *o);
    o->struct_size = sizeof *o;
    o->buttons = f->buttons;
    o->type = f->type;
    o->lx = f->lx; o->ly = f->ly; o->rx = f->rx; o->ry = f->ry;
}

static void neutral(uint32_t type, PSXModPadOutput *o) {
    memset(o, 0, sizeof *o);
    o->struct_size = sizeof *o;
    o->buttons = 0xffff;
    o->type = type;
    o->lx = o->ly = o->rx = o->ry = 0x80;
}

static int output_valid(uint32_t allowed, const PSXModPadOutput *o) {
    return o->struct_size == sizeof *o && o->buttons <= 0xffff &&
           o->type <= PSX_MOD_PAD_NEGCON &&
           (allowed & PSX_MOD_PAD_TYPE_BIT(o->type)) &&
           o->lx <= 255 && o->ly <= 255 && o->rx <= 255 && o->ry <= 255 &&
           o->negcon_i <= 255 && o->negcon_ii <= 255 && o->negcon_l <= 255;
}

int mod_pad_transform_run(uint32_t player, const PSXModPadFrame *frame,
                          const PSXModPadOutput *stock, PSXModPadOutput *out) {
    if (!frame || !out || player >= PSX_MAX_PLAYERS) return 0;
    if (s_release[player] && !s_active[player]) {
        s_release[player] = 0;
        neutral(frame->type, out);
        return 1;
    }
    if (!s_active[player]) return 0;
    PSXModPadOutput o, base;
    if (stock) base = *stock;
    else pass_through(frame, &base);
    o = base;
    if (!s_xf[player].transform(frame, &o)) {
        *out = base;
        return 1;
    }
    if (!output_valid(s_xf[player].allowed_types, &o)) {
        if (s_bad_count[player]++ % BAD_LOG_INTERVAL == 0)
            fprintf(stderr,
                "psxrecomp: mod pad transform (player %u) returned an invalid "
                "frame (struct_size %u, type %u, buttons 0x%x); delivering "
                "neutral (%u so far)\n",
                (unsigned)player, (unsigned)o.struct_size, (unsigned)o.type,
                (unsigned)o.buttons, (unsigned)s_bad_count[player]);
        neutral(frame->type, out);
        return 1;
    }
    *out = o;
    return 1;
}
