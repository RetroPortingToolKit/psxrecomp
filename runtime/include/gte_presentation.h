#ifndef PSX_GTE_PRESENTATION_H
#define PSX_GTE_PRESENTATION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Exact host projection operands that produced one current SXY FIFO entry.
 * This is metadata, never correspondence inferred from the resulting image. */
typedef struct GtePresentationProjection {
    double focal_x,focal_y,center_x,center_y,near_z;
} GtePresentationProjection;
void gte_presentation_capture(int enabled);
int gte_presentation_projection(unsigned fifo,uint32_t xy,uint32_t z,
    GtePresentationProjection *out);
#ifdef __cplusplus
}
#endif
#endif
