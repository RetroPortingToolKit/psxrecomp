/* netplay_fmv_confirm.h - post-movie agreement counting for rollback netplay.
 *
 * After a movie, both peers must agree for RB_FMV_LOCKSTEP_CONFIRM ticks
 * before prediction is unlocked. Agreement is measured in hash-confirmed
 * ticks: each newly confirmed tick past the movie's end counts once, however
 * late its confirmation arrives. A confirmation is always about one RTT
 * behind the tick it confirms, so testing "is sim-1 confirmed yet" every tick
 * never succeeds on the peer that runs ahead. */
#ifndef PSX_NETPLAY_FMV_CONFIRM_H
#define PSX_NETPLAY_FMV_CONFIRM_H

#include <stdint.h>

/* Confirmed ticks gained when the confirmed frontier moves from `from` (the
 * highest tick already counted, or the movie's end) to `resolved`. */
static inline uint32_t rb_fmv_confirm_gain(uint32_t from, uint32_t resolved)
{
    return resolved > from ? resolved - from : 0u;
}

#endif
