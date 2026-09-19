#ifndef PSX_GPU_VK_UPLOAD_H
#define PSX_GPU_VK_UPLOAD_H

#include <stddef.h>

/* R16 VkBufferImageCopy offsets must remain multiples of four bytes. Padding
 * each packed region to an even texel count keeps every following offset
 * aligned without changing the copied image extent. */
static inline size_t vk_upload_aligned_texels(size_t texels)
{
    return (texels + 1u) & ~(size_t)1u;
}

#endif
