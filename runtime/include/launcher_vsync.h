#ifndef PSX_LAUNCHER_VSYNC_H
#define PSX_LAUNCHER_VSYNC_H
/* Launcher ABI is 1=on, 2=off, 3=adaptive, 4=VRR; config is 1,0,-1,2.
 * Older launchers preserve config-only VRR when their row is unchanged. */
static inline int psx_vsync_to_launcher(int value) {
#ifdef RECOMP_LAUNCHER_HAS_VSYNC_VRR
    if (value == 2) return 4;
#endif
    return value == 0 ? 2 : value == -1 ? 3 : 1;
}
static inline int psx_vsync_from_launcher(int value, int previous) {
#ifdef RECOMP_LAUNCHER_HAS_VSYNC_VRR
    if (value == 4) return 2;
#endif
    if (previous == 2 && value == psx_vsync_to_launcher(previous)) return previous;
    return value == 2 ? 0 : value == 3 ? -1 : 1;
}
#endif
