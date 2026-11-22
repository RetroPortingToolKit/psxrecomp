/* Private source-geometry output. Textures are retained copies of raw VRAM.
 * Only the swapchain present consumes the resulting color attachment. */
enum { VK_GEOMETRY_TEXTURES = 4, VK_GEOMETRY_VERTICES = 192000 };
typedef struct VkGeometryImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;
} VkGeometryImage;

typedef struct VkGeometryTexture {
    VkGeometryImage image;
    VkDescriptorSet descriptor;
    uint64_t id;
    uint32_t references;
} VkGeometryTexture;

static struct {
    VkGeometryTexture textures[VK_GEOMETRY_TEXTURES];
    VkDescriptorPool pool;
    VkGeometryImage color, stencil;
    VkFramebuffer framebuffer;
    VkBuffer vertices;
    VkDeviceMemory memory;
    unsigned char *mapped;
    VkFence ready;
    uint64_t output;
    int scale, failed, have_display;
    int display[4];
} s_geometry;
static uint64_t s_geometry_next_id = 1;
static PsxPresentationProvider s_geometry_provider;
static void *s_geometry_provider_user;

static void destroy_geometry_image(VkGeometryImage *image) {
    if (image->view) p_vkDestroyImageView(s_dev, image->view, NULL);
    if (image->image) p_vkDestroyImage(s_dev, image->image, NULL);
    if (image->memory) p_vkFreeMemory(s_dev, image->memory, NULL);
}

static void vk_geometry_shutdown(void) {
    if (s_geometry.pool) p_vkDestroyDescriptorPool(s_dev, s_geometry.pool, NULL);
    for (int i = 0; i < VK_GEOMETRY_TEXTURES; ++i)
        destroy_geometry_image(&s_geometry.textures[i].image);
    if (s_geometry.ready) p_vkDestroyFence(s_dev, s_geometry.ready, NULL);
    if (s_geometry.framebuffer) p_vkDestroyFramebuffer(s_dev, s_geometry.framebuffer, NULL);
    if (s_geometry.mapped) p_vkUnmapMemory(s_dev, s_geometry.memory);
    if (s_geometry.vertices) p_vkDestroyBuffer(s_dev, s_geometry.vertices, NULL);
    if (s_geometry.memory) p_vkFreeMemory(s_dev, s_geometry.memory, NULL);
    destroy_geometry_image(&s_geometry.color);
    destroy_geometry_image(&s_geometry.stencil);
    memset(&s_geometry, 0, sizeof s_geometry);
}

void vk_geometry_set_provider(PsxPresentationProvider provider, void *user) {
    s_geometry_provider = provider;
    s_geometry_provider_user = user;
    vk_geometry_invalidate();
}

void vk_geometry_invalidate(void) {
    s_geometry.have_display = 0;
    s_geometry.output = 0;
}

static int create_geometry_image(VkGeometryImage *image, VkFormat format, int scale,
                                 VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    return make_image(format, VRAM_W * scale, VRAM_H * scale, usage, aspect,
                      &image->image, &image->memory, &image->view);
}

static int create_geometry_resources(void) {
    if (!s_ctx_ok || !s_ready || s_geometry.failed || gpu_display_is_depth24()) return 0;
    if (s_geometry.pool) return s_geometry.scale == s_scale;
    s_geometry.scale = s_scale;

    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_GEOMETRY_TEXTURES};
    VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = VK_GEOMETRY_TEXTURES;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (p_vkCreateDescriptorPool(s_dev, &pool, NULL, &s_geometry.pool) != VK_SUCCESS) goto fail;
    for (int i = 0; i < VK_GEOMETRY_TEXTURES; ++i) {
        VkGeometryTexture *texture = &s_geometry.textures[i];
        if (!create_geometry_image(&texture->image, VK_FORMAT_R16_UINT, 1,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
        VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = s_geometry.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &s_dsl_tex;
        if (p_vkAllocateDescriptorSets(s_dev, &alloc, &texture->descriptor) != VK_SUCCESS) goto fail;
        VkDescriptorImageInfo image = {s_samp, texture->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = texture->descriptor;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        p_vkUpdateDescriptorSets(s_dev, 1, &write, 0, NULL);
    }

    if (!create_geometry_image(&s_geometry.color, VK_FORMAT_R8G8B8A8_UNORM, s_scale,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_geometry_image(&s_geometry.stencil, s_ds_format, s_scale,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, DS_ASPECT))
        goto fail;
    VkImageView views[] = {s_geometry.color.view, s_geometry.stencil.view};
    VkFramebufferCreateInfo framebuffer = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = s_rpass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = views;
    framebuffer.width = VRAM_W * s_scale;
    framebuffer.height = VRAM_H * s_scale;
    framebuffer.layers = 1;
    if (p_vkCreateFramebuffer(s_dev, &framebuffer, NULL, &s_geometry.framebuffer) != VK_SUCCESS) goto fail;
    void *mapped = NULL;
    if (!create_buffer_mapped((VkDeviceSize)VK_GEOMETRY_VERTICES * (sizeof(Vert) + sizeof(TexVert)),
            &s_geometry.vertices, &s_geometry.memory, &mapped)) goto fail;
    s_geometry.mapped = mapped;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (p_vkCreateFence(s_dev, &fence, NULL, &s_geometry.ready) == VK_SUCCESS) return 1;
fail:
    vk_geometry_shutdown();
    s_geometry.failed = 1;
    return 0;
}

static VkGeometryTexture *geometry_texture(uint64_t id) {
    if (!id) return NULL;
    for (int i = 0; i < VK_GEOMETRY_TEXTURES; ++i) {
        VkGeometryTexture *texture = &s_geometry.textures[i];
        if (texture->references && texture->id == id) return texture;
    }
    return NULL;
}

uint64_t vk_geometry_snapshot(void) {
    if (!create_geometry_resources()) return 0;
    VkGeometryTexture *texture = NULL;
    for (int i = 0; i < VK_GEOMETRY_TEXTURES; ++i) {
        if (!s_geometry.textures[i].references) { texture = &s_geometry.textures[i]; break; }
    }
    if (!texture) return 0;
    flush_cpu_upload();
    flush_tex_batch();
    flush_geometry();
    pack_flush();
    VkCommandBuffer cb = begin_oneshot();
    if (!cb) return 0;
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    img_to(cb, texture->image.image, &texture->image.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy = {0};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = (VkExtent3D){VRAM_W, VRAM_H, 1};
    p_vkCmdCopyImage(cb, s_raw_img, s_raw_layout, texture->image.image, texture->image.layout, 1, &copy);
    img_to(cb, s_raw_img, &s_raw_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    img_to(cb, texture->image.image, &texture->image.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    texture->references = 1;
    texture->id = s_geometry_next_id++;
    return texture->id;
}

int vk_geometry_retain(uint64_t version) {
    VkGeometryTexture *texture = geometry_texture(version);
    if (!texture || texture->references == UINT32_MAX) return 0;
    ++texture->references;
    return 1;
}

void vk_geometry_release(uint64_t version) {
    VkGeometryTexture *texture = geometry_texture(version);
    if (texture) --texture->references;
}

static int geometry_same_batch(const PsxPresentationMaterial *a, const PsxPresentationMaterial *b) {
    const uint32_t keys = PSX_PRESENTATION_TEXTURED | PSX_PRESENTATION_MASK_SET | PSX_PRESENTATION_MASK_CHECK;
    return a->semi == b->semi && !((a->flags ^ b->flags) & keys) &&
           memcmp(a->clip, b->clip, sizeof a->clip) == 0 &&
           (!(a->flags & PSX_PRESENTATION_TEXTURED) ||
            (a->texture_version == b->texture_version && a->texture_window == b->texture_window &&
             a->filter == b->filter));
}

static void geometry_tex_vertices(TexVert *out, const PsxPresentationTriangle *triangle,
                                  const PsxPresentationMaterial *material) {
    const uint16_t tpage = material->texpage;
    for (int v = 0; v < 3; ++v) {
        const PsxPresentationDrawVertex *vertex = &triangle->vertices[v];
        out[v] = (TexVert){{
            (float)vertex->x, (float)vertex->y, vertex->uv[0], vertex->uv[1],
            vertex->color[0], vertex->color[1], vertex->color[2], vertex->color[3],
            (float)((tpage & 15) * 64), (float)((tpage & 16) * 16),
            (float)material->clut_x, (float)material->clut_y, (float)((tpage >> 7) & 3),
            (float)((material->flags & PSX_PRESENTATION_RAW_TEXTURE) != 0),
            (float)material->uv_limits[0], (float)material->uv_limits[1],
            (float)material->uv_limits[2], (float)material->uv_limits[3],
            (triangle->flags & PSX_PRESENTATION_AFFINE_UV) ? 0.f : (float)vertex->reciprocal_z,
        }};
    }
}

int vk_geometry_render(const PsxPresentationTriangle *tri, uint32_t count,
                       const PsxPresentationMaterial *mat, uint32_t materials) {
    if (!tri || !mat || !count || count > VK_GEOMETRY_VERTICES / 3 || !create_geometry_resources() ||
        p_vkGetFenceStatus(s_dev, s_geometry.ready) != VK_SUCCESS) return 0;
    int any_mask_check = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (tri[i].resource >= materials) return 0;
        const PsxPresentationMaterial *m = &mat[tri[i].resource];
        if (m->semi < -1 || m->semi > 3 || m->clip[0] < 0 || m->clip[1] < 0 ||
            m->clip[2] < m->clip[0] || m->clip[3] < m->clip[1] ||
            m->clip[2] >= VRAM_W || m->clip[3] >= VRAM_H) return 0;
        if ((m->flags & PSX_PRESENTATION_TEXTURED) &&
            (!geometry_texture(m->texture_version) || m->filter > 1)) return 0;
        if (m->flags & PSX_PRESENTATION_MASK_CHECK) any_mask_check = 1;
        for (int j = 0; j < 3; ++j) {
            const PsxPresentationDrawVertex *v = &tri[i].vertices[j];
            if (!isfinite(v->x) || !isfinite(v->y) ||
                !isfinite(v->uv[0]) || !isfinite(v->uv[1]) ||
                !isfinite(v->reciprocal_z)) return 0;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const PsxPresentationMaterial *m = &mat[tri[i].resource];
        const int texture = (m->flags & PSX_PRESENTATION_TEXTURED) != 0;
        const int mask = (m->flags & PSX_PRESENTATION_MASK_SET) != 0;
        const int check = (m->flags & PSX_PRESENTATION_MASK_CHECK) != 0;
        if (!get_pipeline(texture, 0, texture ? 0 : m->semi + 1, check ? 1 + mask : 0, 0))
            return 0;
        if (texture && (m->semi >= 0 || any_mask_check) &&
            !get_pipeline(1, 0, m->semi + 1, check ? 2 : 0, 0)) return 0;
    }

    VkCommandBuffer cb = begin_oneshot();
    if (!cb) return 0;
    VkGeometryImage *color = &s_geometry.color, *stencil = &s_geometry.stencil;
    img_to(cb, color->image, &color->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue black = {{0.f, 0.f, 0.f, 0.f}};
    const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(cb, color->image, color->layout, &black, 1, &color_range);
    img_to(cb, color->image, &color->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkClearDepthStencilValue zero = {1.f, 0};
    const VkImageSubresourceRange stencil_range = {DS_ASPECT, 0, 1, 0, 1};
    const VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkImageMemoryBarrier ds = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ds.image = stencil->image;
    ds.subresourceRange = stencil_range;
    ds.srcQueueFamilyIndex = ds.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ds.oldLayout = stencil->layout;
    ds.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ds.srcAccessMask = stencil->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    ds.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, tests, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ds);
    p_vkCmdClearDepthStencilImage(cb, stencil->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &zero, 1, &stencil_range);
    stencil->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    ds.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ds.newLayout = stencil->layout;
    ds.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ds.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, tests, 0, 0, NULL, 0, NULL, 1, &ds);
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s_rpass;
    rp.framebuffer = s_geometry.framebuffer;
    rp.renderArea.extent = (VkExtent2D){VRAM_W * s_scale, VRAM_H * s_scale};
    p_vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    set_vp_full(cb);

    const VkDeviceSize flat_offset = 0, tex_offset = sizeof(Vert) * VK_GEOMETRY_VERTICES;
    Vert *flat = (Vert *)s_geometry.mapped;
    TexVert *textured = (TexVert *)(s_geometry.mapped + tex_offset);
    for (uint32_t first = 0; first < count;) {
        const PsxPresentationMaterial *m = &mat[tri[first].resource];
        const int textured_batch = (m->flags & PSX_PRESENTATION_TEXTURED) != 0;
        const int mask = (m->flags & PSX_PRESENTATION_MASK_SET) != 0;
        const int check = (m->flags & PSX_PRESENTATION_MASK_CHECK) != 0;
        uint32_t last = first + 1;
        if (!textured_batch || (m->semi < 0 && !any_mask_check)) {
            while (last < count && geometry_same_batch(m, &mat[tri[last].resource])) ++last;
        }
        const uint32_t base = first * 3, vertices = (last - first) * 3;
        set_scissor_px(cb, m->clip[0], m->clip[1],
                       m->clip[2] - m->clip[0] + 1, m->clip[3] - m->clip[1] + 1);
        if (textured_batch) {
            for (uint32_t i = first; i < last; ++i)
                geometry_tex_vertices(textured + i * 3, &tri[i], &mat[tri[i].resource]);
            const VkDescriptorSet descriptor = geometry_texture(m->texture_version)->descriptor;
            p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl_tex, 0, 1, &descriptor, 0, NULL);
            p_vkCmdBindVertexBuffers(cb, 0, 1, &s_geometry.vertices, &tex_offset);
            TexPush push = {0};
            const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            const int split = m->semi >= 0 || any_mask_check;
            push.shift = px_shift();
            push.xhalf = 512.f;
            push.maskset = mask;
            push.filter = m->filter;
            for (int j = 0; j < 4; ++j) push.twin[j] = (m->texture_window >> (5 * j)) & 31;
            push.semipass = split ? 1 : 0;
            bind_masked(cb, 1, 0, 0, check, mask);
            p_vkCmdPushConstants(cb, s_pl_tex, stages, 0, sizeof push, &push);
            p_vkCmdDraw(cb, vertices, 1, base, 0);
            if (split) {
                push.semipass = 2;
                bind_masked(cb, 1, 0, m->semi + 1, check, 1);
                p_vkCmdPushConstants(cb, s_pl_tex, stages, 0, sizeof push, &push);
                p_vkCmdDraw(cb, vertices, 1, base, 0);
            }
        } else {
            for (uint32_t i = first; i < last; ++i)
                for (int j = 0; j < 3; ++j) {
                    const PsxPresentationDrawVertex *v = &tri[i].vertices[j];
                    flat[i * 3 + j] = (Vert){(float)v->x, (float)v->y,
                                             v->color[0], v->color[1], v->color[2], (float)mask};
                }
            const GeoPush push = {px_shift(), 0.f, 512.f};
            bind_masked(cb, 0, 0, m->semi + 1, check, mask);
            p_vkCmdPushConstants(cb, s_pl_geo, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
            p_vkCmdBindVertexBuffers(cb, 0, 1, &s_geometry.vertices, &flat_offset);
            p_vkCmdDraw(cb, vertices, 1, base, 0);
        }
        first = last;
    }
    p_vkCmdEndRenderPass(cb);
    img_to(cb, color->image, &color->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    s_geometry.output = 0;
    if (p_vkResetFences(s_dev, 1, &s_geometry.ready) != VK_SUCCESS ||
        !submit_work(s_geometry.ready)) {
        s_geometry.failed = 1;
        return 0;
    }
    s_geometry.output = s_geometry_next_id++;
    return 1;
}

uint64_t vk_geometry_output_version(void) { return s_geometry.output; }

int vk_geometry_output_ready(uint64_t version) {
    return version && version == s_geometry.output && s_geometry.scale == s_scale &&
           p_vkGetFenceStatus(s_dev, s_geometry.ready) == VK_SUCCESS;
}

static int vk_geometry_source(int x, int y, int w, int h, int force_4_3, int crop[4],
                              VkImage *image, VkImageView *view, VkImageLayout **layout) {
    if (!s_geometry_provider) return 0;
    const int display[] = {x, y, w, h};
    const int repeated = s_geometry.have_display &&
                         memcmp(s_geometry.display, display, sizeof display) == 0;
    memcpy(s_geometry.display, display, sizeof display);
    s_geometry.have_display = !force_4_3;
    int selected[4] = {0};
    if (!repeated || force_4_3 || !vk_geometry_output_ready(s_geometry.output) ||
        !s_geometry_provider(s_geometry_provider_user, x, y, w, h, repeated, selected) ||
        selected[0] < 0 || selected[1] < 0 || selected[2] != w || selected[3] != h ||
        selected[0] > VRAM_W - w || selected[1] > VRAM_H - h) return 0;
    memcpy(crop, selected, sizeof selected);
    *image = s_geometry.color.image;
    *view = s_geometry.color.view;
    *layout = &s_geometry.color.layout;
    return 1;
}

/* Debug export reads only the private image. Live presentation never reads it back. */
int vk_geometry_read_rgba(uint8_t *out, size_t capacity, int x, int y, int w, int h) {
    if (!out || !s_geometry.output || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x > VRAM_W - w || y > VRAM_H - h || (size_t)w * h > capacity / 4) return 0;
    const int scale = s_geometry.scale;
    const size_t stride = (size_t)w * scale * 4;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    if (!make_staging(stride * h * scale, &buffer, &memory, &mapped)) return 0;
    VkCommandBuffer cb = begin_oneshot();
    if (!cb) { free_staging(buffer, memory); return 0; }
    VkGeometryImage *color = &s_geometry.color;
    img_to(cb, color->image, &color->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = (VkOffset3D){x * scale, y * scale, 0};
    copy.imageExtent = (VkExtent3D){w * scale, h * scale, 1};
    p_vkCmdCopyImageToBuffer(cb, color->image, color->layout, buffer, 1, &copy);
    VkBufferMemoryBarrier visible = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    visible.buffer = buffer;
    visible.size = VK_WHOLE_SIZE;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                          0, NULL, 1, &visible, 0, NULL);
    img_to(cb, color->image, &color->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_oneshot(cb);
    gpu_sync();
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            memcpy(out + ((size_t)row * w + col) * 4,
                   (const uint8_t *)mapped + (size_t)(row * scale) * stride + col * scale * 4u, 4);
    free_staging(buffer, memory);
    return 1;
}
