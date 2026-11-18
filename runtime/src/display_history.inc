/* Shared production display-history adapter; included by debug_server.c. */
#define DISP_RING_CAP   64    /* full-VRAM entries are 1MB each; ~1s of
                               * lookback for exact-frame forensics */
/* The entire 1024x512 raw VRAM as the renderer sees it at this frame (GL: FBO
 * truth; SW and depth24: CPU truth), so an offline client can micro-cosim ANY
 * prim of the frame — rasterize it from the GP0 ring against the exact
 * same-frame texels/CLUTs — and attribute a bad pixel to sampling vs content.
 * Streamed texture pages and palette cycling make later peeks worthless; only
 * a same-frame capture is evidence. */
typedef struct {
    uint32_t frame;
    uint8_t  valid;
    GpuDisplayInfo di;            /* display geometry/depth at capture */
    uint16_t *vram;               /* full 1024x512 */
#if defined(PSX_GPU_DISPLAY_HISTORY) && PSX_GPU_DISPLAY_HISTORY
    _Atomic int lock;
    uint64_t gpu_version;
#endif
} DispRingEntry;
static PSX_BSS DispRingEntry s_disp_ring[DISP_RING_CAP];
/* Capture mark state (debug_server_capture_mark / capture_mark). */
static int      s_capture_frozen = 0;
static uint64_t s_capture_mark_frame = 0;
static uint64_t s_capture_mark_count = 0;
static char     s_capture_wide_path[512];
static int      s_capture_wide_ok = 0;
static uint16_t     *s_disp_ring_px = NULL;   /* one block for all entries */

#if defined(PSX_GPU_DISPLAY_HISTORY) && PSX_GPU_DISPLAY_HISTORY
/* Capture metadata can be completed by the GL worker. Never hold this short
 * lock across a transfer of GL ownership; ring readers drain that worker first. */
static void disp_ring_lock(DispRingEntry *e) {
    while(atomic_exchange_explicit(&e->lock,1,memory_order_acquire)) {}
}
static void disp_ring_unlock(DispRingEntry *e) {
    atomic_store_explicit(&e->lock,0,memory_order_release);
}
static void disp_ring_gpu_ready(void *token,uint64_t version) {
    const uint32_t frame=(uint32_t)(uintptr_t)token;
    DispRingEntry *e=&s_disp_ring[frame%DISP_RING_CAP];
    disp_ring_lock(e);
    if(e->frame==frame) {
        e->gpu_version=version;
        e->valid=version!=0 || gl_renderer_fbo_peek(0,0,1024,512,e->vram);
        disp_ring_unlock(e);
    } else {
        disp_ring_unlock(e);
        if(version)psx_presentation_gpu_release(version);
    }
}
static int disp_ring_materialize(DispRingEntry *e) {
    if(e->gpu_version) {
        if(!psx_presentation_gpu_snapshot_read_vram(e->gpu_version,e->vram))return 0;
        psx_presentation_gpu_release(e->gpu_version);
        e->gpu_version=0;
    }
    return e->valid!=0;
}
static void disp_ring_preserve_gpu(void) {
    /* Context restart is infrequent; preserve the complete late-probe window
     * before destroying its GPU objects. At most 64 MiB are read once here. */
    for(unsigned i=0;i<DISP_RING_CAP;++i) {
        DispRingEntry *e=&s_disp_ring[i];
        if(e->gpu_version && !disp_ring_materialize(e)) {
            fprintf(stderr,"display history: cannot preserve frame %u at context teardown\n",e->frame);
            e->valid=0;e->gpu_version=0;
        }
    }
}
#else
static int disp_ring_materialize(DispRingEntry *e) { return e->valid!=0; }
#endif

static void disp_ring_capture(void)
{
    /* Native GPU history stays enabled by default. The legacy full-VRAM
     * readback path is opt-in during threaded rendering; explicit overrides
     * still control both paths, independently of input/replay recording. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_DISPLAY_RING");
        enabled = (e && *e) ? (*e != '0') : 2;
    }
    if (!enabled || s_capture_frozen) return;
#if !defined(PSX_GPU_DISPLAY_HISTORY) || !PSX_GPU_DISPLAY_HISTORY
    if (enabled == 2 && gl_renderer_render_thread_active()) return;
#endif
    if (!s_disp_ring_px) {
        size_t vram = (size_t)1024 * 512;
        s_disp_ring_px = (uint16_t *)malloc(DISP_RING_CAP * vram * sizeof(uint16_t));
        if (!s_disp_ring_px) return;
        for (int i = 0; i < DISP_RING_CAP; i++)
            s_disp_ring[i].vram = s_disp_ring_px + (size_t)i * vram;
    }
    DispRingEntry *e = &s_disp_ring[(uint32_t)(s_frame_count % DISP_RING_CAP)];
#if defined(PSX_GPU_DISPLAY_HISTORY) && PSX_GPU_DISPLAY_HISTORY
    disp_ring_lock(e);
    const uint64_t previous=e->gpu_version;
    e->gpu_version=0;
#endif
    e->frame = (uint32_t)s_frame_count;
    e->valid = 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
#if defined(PSX_GPU_DISPLAY_HISTORY) && PSX_GPU_DISPLAY_HISTORY
    e->di=di;
    disp_ring_unlock(e);
    if(di.disabled || di.width==0 || di.height==0) {
        if(previous)psx_presentation_gpu_release(previous);
        return;
    }
    gpu_timeline_note(GTL_DISP_CAPTURE,0,di.display_x|(di.display_y<<16));
    if(!di.depth24 && psx_presentation_gpu_history_replace_async(previous,
        disp_ring_gpu_ready,(void *)(uintptr_t)e->frame))return;
    if(previous)psx_presentation_gpu_release(previous);
#endif
    if (di.disabled || di.width == 0 || di.height == 0) return;
#if !defined(PSX_GPU_DISPLAY_HISTORY) || !PSX_GPU_DISPLAY_HISTORY
    gpu_timeline_note(GTL_DISP_CAPTURE, 0, di.display_x | (di.display_y << 16));
#endif
    e->di = di;
    /* GPU-side truth when the GL raster pipeline is live; CPU VRAM otherwise
     * (the software backend's authoritative surface). depth24 always reads
     * the CPU mirror: packed RGB888 MDEC bytes live there, not in the FBO
     * (see screenshot_file). Runs on the present thread, where the GL context
     * is current. Deferred: with the render thread on, the readback is
     * recorded at this point of the command stream and filled in by the
     * render thread (ring readers sync first). */
    if (di.depth24 || !gl_renderer_fbo_peek_deferred(0, 0, 1024, 512, e->vram)) {
        const uint16_t *v = gpu_get_vram();
        memcpy(e->vram, v, (size_t)1024 * 512 * sizeof(uint16_t));
    }
    e->valid = 1;
}

/* Pixel history is independent of the TCP listener and full debug CPU-state
 * recording. Both shipping and development call it before advancing frame. */
void debug_server_record_display_frame(void) {
#if defined(PSX_GPU_DISPLAY_HISTORY) && PSX_GPU_DISPLAY_HISTORY
    static int registered;
    if(!registered) {
        psx_presentation_gpu_history_set_shutdown(disp_ring_preserve_gpu);
        registered=1;
    }
#endif
    disp_ring_capture();
}

