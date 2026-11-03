/* Existing GL fixtures deliberately run without any external pack. The HD
 * fixture links the real session adapter instead. */
/* These renderer fixtures also run without a trusted title profile. */
static int test_title_profile_active, test_title_profile_banks;
int psx_mod_netplay_is_active(void) { return test_title_profile_active; }
int psx_mod_netplay_requires_texture_banks(void) { return test_title_profile_banks; }
#ifndef PSX_TEST_HD_TEXTURE_PACK
int gpu_hd_textures_acquire_draw(uint16_t tp, uint16_t cx, uint16_t cy,
    const int limits[4], uint32_t window, int semi, GpuHdTextureImage* image) {
    (void)tp; (void)cx; (void)cy; (void)limits; (void)window; (void)semi;
    memset(image, 0, sizeof(*image)); return 0;
}
void gpu_hd_textures_release_image(GpuHdTextureImage* image) {
    memset(image, 0, sizeof(*image));
}
void gpu_hd_textures_note_applied(void) {}
void gpu_hd_textures_note_gl_cache(int eviction) { (void)eviction; }
int gpu_hd_textures_acquire_gl_draw(uint16_t tp, uint16_t cx, uint16_t cy,
    const int limits[4], uint32_t window, int semi, GpuHdTextureImage* image) {
    return gpu_hd_textures_acquire_draw(tp, cx, cy, limits, window, semi, image);
}
void gpu_hd_textures_reset_tracking(void) {}
void gpu_hd_textures_restage(void) {}
void gpu_hd_textures_bind_vram(const uint16_t* vram) { (void)vram; }
void gpu_hd_textures_suppress_dumps(int on) { (void)on; }
#endif
