#ifndef PSX_LAUNCHER_VSYNC_H
#define PSX_LAUNCHER_VSYNC_H
/* Launcher ABI is 1=on, 2=off, 3=adaptive; config/GL is 1,0,-1.
 * Keep an unsupported config-only mode (VRR=2) if the row stays unchanged. */
static inline int psx_vsync_to_launcher(int value) {
    return value == 0 ? 2 : value == -1 ? 3 : 1;
}
static inline int psx_vsync_from_launcher(int value, int previous) {
    if (previous == 2 && value == psx_vsync_to_launcher(previous)) return previous;
    return value == 2 ? 0 : value == 3 ? -1 : 1;
}
#endif
