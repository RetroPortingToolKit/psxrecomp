#ifndef PSX_WS_VIEW_ANCHOR_H
#define PSX_WS_VIEW_ANCHOR_H

/* Host presentation geometry. Camera bounds describe the native view's left
 * edge; the room's right edge is camera_max + native_width. */
typedef struct WsViewAnchor {
    int left, right;
    int shift;
    int pad_left, pad_right;
} WsViewAnchor;

static inline WsViewAnchor ws_view_anchor(int extra, int camera,
                                         int camera_min, int camera_max) {
    WsViewAnchor v = {extra, extra, 0, 0, 0};
    if (extra <= 0 || camera_min > camera_max ||
        camera < camera_min || camera > camera_max) return v;
    int avail_left = camera - camera_min;
    int avail_right = camera_max - camera;
    int total = 2 * extra;
    v.left = avail_left < extra ? avail_left : extra;
    v.right = avail_right < total - v.left ? avail_right : total - v.left;
    v.left = avail_left < total - v.right ? avail_left : total - v.right;
    int padding = total - v.left - v.right;
    v.pad_left = padding / 2;
    v.pad_right = padding - v.pad_left;
    v.shift = v.left + v.pad_left - extra;
    return v;
}
#endif
