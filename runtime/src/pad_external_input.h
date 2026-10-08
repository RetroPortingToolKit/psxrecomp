#pragma once
/* Ordered external-input resolution for one offline player's pad.
 *
 *   (1) physical/local capture  -> buttons, device presence
 *   (2) offline controller source (psx_mod_set_controller_source), if any:
 *         buttons = source & physical; sticks/type from the source, through
 *         the controller presentation policy (hook)
 *   (3) local mouse policy (psx_mod_set_local_mouse_policy), P1 only: may
 *         override ONLY the right analog axes of whatever (1)/(2) produced.
 *
 * Precedence: a source never suppresses the mouse policy. The mouse sees the
 * RESOLVED pad (final buttons, final analog flag, final right stick), so a
 * capture is never taken and then discarded, and an active source whose right
 * stick is deflected, whose pad is digital, or whose Start is held disables
 * the mouse exactly as a physical stick/digital pad/Start would. The mouse
 * is reset (capture released) on a detach/reset release frame, no device, or
 * an armed savestate input guard. A declined/invalid source stays attached and
 * supplies neutral source state; the physical buttons and P1 mouse still fold
 * onto that neutral sample under the ordinary eligibility rules.
 *
 * Pure: every side effect goes through PadExtHooks, so the order is testable
 * without SDL/SIO. Main (emulation) thread only. */
#include <stdint.h>
#include "mod_plugins.h"
#include "psx_netplay.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PadExtHooks {
    void *ctx;
    /* Input guard (savestate close/load): all external input delivers
     * neutral and the mouse is reset. Queried once per resolve. */
    int (*guard_active)(void *ctx);
    /* (1) Physical capture of port `s`; NO mouse side effects. Returns 1 if a
     * device is present, else 0 (*out released/disconnected). With
     * `guarded`, buttons/sticks are neutral but type/presence still resolve. */
    int (*capture_local)(void *ctx, int s, PsxNetPad *out, int guarded);
    /* (2) Source for port `s`: returns 0 when none; else fills *state and
     * *released (a one-frame neutral release after detach/reset). */
    int (*source_sample)(void *ctx, int s, PSXModControllerState *state, int *released);
    /* Release frame: neutral pad + the port's real SIO state restored. */
    void (*source_release)(void *ctx, int s, PsxNetPad *out);
    /* Live source frame: pad->buttons is already source&physical; fill
     * sticks/analog (presentation policy, multitap rule) and claim the port. */
    void (*source_resolve)(void *ctx, int s, const PSXModControllerState *state,
                           int guarded, PsxNetPad *pad);
    /* (3) Mouse policy. Only P1 (s == 0) is ever offered to it. */
    void (*mouse_reset)(void *ctx);
    void (*mouse_fold)(void *ctx, int connected, int analog, uint16_t buttons,
                       uint8_t *rx, uint8_t *ry);
} PadExtHooks;

/* Resolve one port. Returns 1 = deliver *out, 0 = no device/source here. */
int pad_ext_resolve(const PadExtHooks *h, int s, PsxNetPad *out);

/* Host conditions under which NO external input (mouse capture) may be live.
 * Mirrors the gates every other local-only feature obeys. 1 = live. */
typedef struct PadExtGate {
    int injected_input;      /* debug-server set_input / axis override */
    int headless;
    int netplay_active;
    int netplay_resim;       /* rollback resimulation */
    int selfcheck_locked;    /* selfcheck record/replay input lock */
    int selfcheck_resim;
    int render_pass;         /* eye / extra render pass */
    int savestate_menu_open;
    int rewind_open;
    int input_guard;         /* savestate input guard armed */
} PadExtGate;
int pad_ext_live(const PadExtGate *g);

#ifdef __cplusplus
}
#endif
