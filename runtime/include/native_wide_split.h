#ifndef PSX_NATIVE_WIDE_SPLIT_H
#define PSX_NATIVE_WIDE_SPLIT_H

/* Presentation mapping for two authored camera halves. The optional one-pixel
 * inset at each inner edge preserves the game's central divider. Other draw
 * areas (including whole-frame overlays) retain the ordinary wide mapping. */
typedef struct NativeWideSplit {
    int source_x, source_width;
    int target_x, target_width;
    int shift, margin;
    int clip_left, clip_right;
} NativeWideSplit;

static inline int native_wide_split(int half, int wide_width, int base,
                                    int area_left, int area_right,
                                    NativeWideSplit *out) {
    int slot, inset_left = 0, inset_right = 0;
    if (half <= 1 || half > 512 || base < 0 || base > 1024 - 2 * half ||
        wide_width <= 2 * half || ((wide_width - 2 * half) & 3)) return 0;
    if (area_left == base &&
        (area_right == base + half - 1 || area_right == base + half - 2)) {
        slot = 0;
        inset_right = base + half - 1 - area_right;
    } else if (area_right == base + 2 * half - 1 &&
               (area_left == base + half || area_left == base + half + 1)) {
        slot = 1;
        inset_left = area_left - (base + half);
    } else return 0;
    if (out) {
        out->source_x = base + slot * half;
        out->source_width = half;
        out->target_x = slot ? wide_width / 2 : 0;
        out->target_width = wide_width / 2;
        out->margin = (out->target_width - half) / 2;
        out->shift = out->target_x + out->margin - out->source_x;
        out->clip_left = out->target_x + inset_left;
        out->clip_right = out->target_x + out->target_width - 1 - inset_right;
    }
    return 1;
}
#endif
