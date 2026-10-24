#pragma once
/* Renderer fixtures have no guest CPU; exercise the neutral unavailable projection.
 * Atomic publication is covered independently by postfx_publication_test. */
#include "gte_postfx_projection.h"
void gte_postfx_projection_enable(int enabled) {(void)enabled;}
int gte_postfx_projection_read(GtePostfxProjection *out) {(void)out;return 0;}
