#ifndef PSX_WS_BACKDROP_MARGIN_H
#define PSX_WS_BACKDROP_MARGIN_H
#include <stdint.h>

/* Round outward: even a partial newly visible column must be submitted. The
 * caller includes its screen-space guard in reveal_pixels. The guest's own
 * row clamps still enforce finite level bounds. */
static inline int psx_ws_backdrop_columns(int window_columns, int reveal_pixels,
                                         int native_width) {
    if (window_columns <= 0 || reveal_pixels <= 0 || native_width <= 0) return 0;
    int64_t columns = ((int64_t)window_columns * reveal_pixels + native_width - 1)
                    / native_width;
    /* The detector's consumers narrow to signed 16-bit indices. More than
     * 0x3fff extra columns cannot reveal anything further in byte-count rows. */
    return columns > 0x3fff ? 0x3fff : (int)columns;
}

/* Some segmented rows clamp START before their final table bias. Widening
 * that final bound must not bypass index zero or wrap the signed-16-bit END
 * before the guest's finite-table upper clamp executes. */
static inline uint32_t psx_ws_backdrop_bound(uint32_t orig, int is_end, int margin) {
    int64_t value = (int32_t)orig;
    value += is_end ? margin : -margin;
    if (value < 0) value = 0;
    if (value > 0x7fff) value = 0x7fff;
    return (uint32_t)value;
}
#endif
