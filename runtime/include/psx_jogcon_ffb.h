/* psx_jogcon_ffb.h - JogCon force feedback onto a host wheel's haptics.
 *
 * The JogCon's motor takes one byte per 0x42 poll at the motor slot the game
 * mapped with 0x4D: high nibble = command, low nibble = strength (0..15).
 * Command 0 stops the motor; 1 and 2 turn the wheel one way or the other;
 * any other command holds it against the player's turn. A host wheel with
 * SDL haptics gets the same thing: a constant force for 1 / 2 (signed by
 * direction, scaled by strength) and a centring spring for a hold. Gamepads
 * are not driven (no wheel to turn). EXPERIMENTAL, opt-in per title with
 * game.toml [controller] experimental_jogcon_force_feedback = true. Which way
 * 1 and 2 turn has not been checked on JogCon hardware or a physical wheel;
 * experimental_jogcon_force_feedback_invert flips it.
 *
 * Neutral by construction: every force is played with a finite lifetime
 * (PSX_JOGCON_FFB_LIFETIME_MS) and renewed each VBlank, so a pause, stall or
 * crash lets the wheel go slack on its own; pause loops, disconnects and
 * session end also stop it explicitly (psx_jogcon_ffb_neutral).
 * Pure (no SDL): the driver talks to a backend, so tests use a recorder. */
#ifndef PSX_JOGCON_FFB_H
#define PSX_JOGCON_FFB_H

#include <stdint.h>

#define PSX_JOGCON_MOTOR_STOP  0u
#define PSX_JOGCON_MOTOR_CW    1u
#define PSX_JOGCON_MOTOR_CCW   2u

typedef struct PsxJogconFfb {
    int16_t  constant;   /* SDL constant-force level, -32767..32767 */
    uint16_t spring;     /* SDL spring coefficient, 0..32767 */
} PsxJogconFfb;

static inline PsxJogconFfb psx_jogcon_ffb_map(uint8_t command, uint8_t strength,
                                              int invert) {
    PsxJogconFfb f = {0, 0};
    const int level = (int)(strength & 0x0Fu) * 32767 / 15;
    command &= 0x0Fu;
    if (command == PSX_JOGCON_MOTOR_STOP || !level) return f;
    if (command == PSX_JOGCON_MOTOR_CW || command == PSX_JOGCON_MOTOR_CCW) {
        int v = command == PSX_JOGCON_MOTOR_CW ? level : -level;
        if (invert) v = -v;
        f.constant = (int16_t)v;
    } else {
        f.spring = (uint16_t)level;
    }
    return f;
}

/* Effect lifetime: a force not renewed within this ends by itself. */
#define PSX_JOGCON_FFB_LIFETIME_MS 120u

typedef struct PsxJogconFfbBackend {
    void *ctx;
    /* Play / renew f (nonzero) for ms milliseconds. */
    void (*play)(void *ctx, const PsxJogconFfb *f, uint32_t ms);
    /* Neutral: stop every force now. */
    void (*stop)(void *ctx);
} PsxJogconFfbBackend;

typedef struct PsxJogconFfbDriver {
    PsxJogconFfb last;
    int active;      /* a force is playing on the backend */
} PsxJogconFfbDriver;

/* One VBlank. enabled = title opt-in and a JogCon presented on this port. */
static inline void psx_jogcon_ffb_tick(PsxJogconFfbDriver *d,
                                       const PsxJogconFfbBackend *b,
                                       int enabled, uint8_t command,
                                       uint8_t strength, int invert) {
    PsxJogconFfb f = {0, 0};
    if (!d || !b) return;
    if (enabled) f = psx_jogcon_ffb_map(command, strength, invert);
    if (f.constant || f.spring) {
        if (b->play) b->play(b->ctx, &f, PSX_JOGCON_FFB_LIFETIME_MS);
        d->active = 1;
    } else if (d->active) {
        if (b->stop) b->stop(b->ctx);
        d->active = 0;
    }
    d->last = f;
}

/* Pause, disconnect, session end: stop now (idempotent). */
static inline void psx_jogcon_ffb_neutral(PsxJogconFfbDriver *d,
                                          const PsxJogconFfbBackend *b) {
    if (!d) return;
    if (d->active && b && b->stop) b->stop(b->ctx);
    d->active = 0;
    d->last.constant = 0;
    d->last.spring = 0;
}

#endif /* PSX_JOGCON_FFB_H */
