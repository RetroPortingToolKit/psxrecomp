#ifndef PSXRECOMP_MOD_VISIBLE_PLACEMENTS_H
#define PSXRECOMP_MOD_VISIBLE_PLACEMENTS_H
#include <stdint.h>

/* Authored placement visibility, independent of a title's actor allocator.
 * Rectangles use the strict bounds of the original Capcom placement scan.
 * Caller stores its visited bitmap/previous rectangle in snapshotted mod RAM. */
typedef struct PSXPlacementRect { int32_t left,right,top,bottom; } PSXPlacementRect;
static inline int psx_placement_contains(PSXPlacementRect r,int32_t x,int32_t y) {
    return x>r.left && x<r.right && y>r.top && y<r.bottom;
}
/* Return only newly exposed strips. Overlapping edge coordinates are included
 * in the strip bounds because the original scanner excludes its endpoints. */
static inline unsigned psx_placement_exposed(PSXPlacementRect old,PSXPlacementRect now,
                                            int valid,PSXPlacementRect out[4]) {
    if (!valid || now.left>=old.right || now.right<=old.left ||
        now.top>=old.bottom || now.bottom<=old.top) { out[0]=now;return 1; }
    unsigned n=0;
    if(now.left<old.left)out[n++]=(PSXPlacementRect){now.left,old.left+1,now.top,now.bottom};
    if(now.right>old.right)out[n++]=(PSXPlacementRect){old.right-1,now.right,now.top,now.bottom};
    int32_t left=now.left>old.left?now.left:old.left,right=now.right<old.right?now.right:old.right;
    if(now.top<old.top)out[n++]=(PSXPlacementRect){left,right,now.top,old.top+1};
    if(now.bottom>old.bottom)out[n++]=(PSXPlacementRect){left,right,old.bottom-1,now.bottom};
    return n;
}
#endif
