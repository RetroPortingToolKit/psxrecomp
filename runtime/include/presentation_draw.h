#ifndef PSX_PRESENTATION_DRAW_H
#define PSX_PRESENTATION_DRAW_H

#include "presentation_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reusable, host-only geometry renderer front end. The output preserves newer
 * scene draw order and opaque resource indices. A backend draws these triangles
 * into its own presentation target, never guest VRAM. No graphics API, guest
 * state, image correspondence or wall-clock dependency enters this stage. */
typedef struct PsxPresentationDraw PsxPresentationDraw;
typedef struct PsxPresentationViewport {
    double left, top, right, bottom; /* projection coordinates, Y increases down */
} PsxPresentationViewport;
typedef struct PsxPresentationDrawVertex {
    double x, y, reciprocal_z;
    float uv[2];
    float color[4]; /* R,G,B,A; affine screen interpolation, as on the PS1 */
} PsxPresentationDrawVertex;
typedef struct PsxPresentationTriangle {
    uint64_t identity;
    uint32_t resource, flags;
    PsxPresentationDrawVertex vertices[3];
} PsxPresentationTriangle;

/* Fixed capacities, allocated once. Clipping one source triangle may create up
 * to six triangles. Exceeding either capacity fails the entire build: the caller
 * must use its ordinary native presentation, never publish partial geometry. */
PsxPresentationDraw *psx_presentation_draw_create(uint32_t source_vertices,
    uint32_t triangles);
void psx_presentation_draw_destroy(PsxPresentationDraw *draw);
int psx_presentation_draw_build(PsxPresentationDraw *draw,
    const PsxPresentationScene *old_scene, const PsxPresentationScene *new_scene,
    double alpha, const PsxPresentationViewport *viewport);
/* The returned array is owned by draw, valid until the next build/destroy.
 * Resource indices refer to new_scene, which must remain sealed and alive
 * through backend consumption. No resource pointer is borrowed by draw. A
 * successful, entirely clipped frame has count zero and a non-NULL array. */
const PsxPresentationTriangle *psx_presentation_draw_triangles(
    const PsxPresentationDraw *draw, uint32_t *count);

#ifdef __cplusplus
}
#endif
#endif
