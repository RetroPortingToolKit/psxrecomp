#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GtePostfxProjection { int32_t h, ofx, ofy; } GtePostfxProjection;
/* Coherent latest CPU projection, not a frame/view identity. One CPU producer. */
void gte_postfx_projection_enable(int enabled);
int gte_postfx_projection_read(GtePostfxProjection *out);
#ifdef __cplusplus
}
#endif
