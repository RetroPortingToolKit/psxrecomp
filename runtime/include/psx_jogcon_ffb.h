/* psx_jogcon_ffb.h - JogCon force feedback onto a host wheel's haptics.
 *
 * The JogCon's motor takes one byte per 0x42 poll at the motor slot the game
 * mapped with 0x4D: high nibble = command, low nibble = strength (0..15).
 * Command 0 stops the motor; 1 and 2 turn the wheel one way or the other;
 * any other command holds it against the player's turn. A host wheel with
 * SDL haptics gets the same thing: a constant force for 1 / 2 (signed by
 * direction, scaled by strength) and a centring spring for a hold. Gamepads
 * are not driven (no wheel to turn). Opt-in per title with game.toml
 * [controller] jogcon_force_feedback = true. Which way 1 and 2 turn has not
 * been checked on JogCon hardware; jogcon_force_feedback_invert flips it.
 * Pure (no SDL), so tests link it directly. */
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

#endif /* PSX_JOGCON_FFB_H */
