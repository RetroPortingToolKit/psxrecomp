#ifndef PSX_PRESENTATION_VK_H
#define PSX_PRESENTATION_VK_H

#include "presentation_gpu.h"

/* Source geometry runs on the Vulkan owner in native GPU command order. */
void vk_geometry_set_provider(PsxPresentationProvider provider, void *user);
void vk_geometry_invalidate(void);
uint64_t vk_geometry_snapshot(void);
int vk_geometry_retain(uint64_t version);
void vk_geometry_release(uint64_t version);
int vk_geometry_render(const PsxPresentationTriangle *triangles, uint32_t count,
                       const PsxPresentationMaterial *materials, uint32_t material_count);
uint64_t vk_geometry_output_version(void);
int vk_geometry_output_ready(uint64_t version);
int vk_geometry_read_rgba(uint8_t *out, size_t capacity, int x, int y, int w, int h);

#endif
