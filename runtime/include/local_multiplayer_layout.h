#ifndef PSX_LOCAL_MULTIPLAYER_LAYOUT_H
#define PSX_LOCAL_MULTIPLAYER_LAYOUT_H

/* Bottom-origin presenter coordinates inside a letterboxed session canvas.
 * Three seats keep a native-width lower view centered between black pillars. */
typedef struct LocalMultiplayerRect { int x, y, w, h; } LocalMultiplayerRect;
static inline LocalMultiplayerRect local_multiplayer_rect(unsigned views, unsigned seat,
                                                          int width, int height) {
    LocalMultiplayerRect r = {0, 0, width / 2, height};
    if (views >= 3) {
        r.h = height / 2;
        if (seat < 2) { r.y = height / 2; r.h = height - r.y; }
    }
    if (views == 3 && seat == 2) r.x = (width - r.w) / 2;
    else if (seat & 1) { r.x = width / 2; r.w = width - r.x; }
    return r;
}
#endif
