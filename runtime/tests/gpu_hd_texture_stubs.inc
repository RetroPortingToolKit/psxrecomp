/* Existing GL fixtures deliberately run without any external pack. The HD
 * fixture links the real session adapter instead. */
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
void gpu_hd_textures_reset_tracking(void) {}
#endif
