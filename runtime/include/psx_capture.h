#ifndef PSX_CAPTURE_H
#define PSX_CAPTURE_H

/*
 * Unattended presented-frame / audio capture (--capture-out and friends).
 *
 * The runtime opens its normal presenter in a never-shown window sized to
 * --capture-size, so widescreen/adaptive projection, internal resolution,
 * PGXP, supersampling, bezels and HUD anchoring run exactly as they do in a
 * visible window. Each guest vblank the composed back buffer is read back and
 * written as frame_NNNNNN.png|ppm; the SPU's final 44.1 kHz stereo mix is
 * written to a WAV. Pacing, vsync, the host audio device, host OSD and real
 * keyboard/controller input are all off. Input comes from --input-script.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Command line: consumes argv[*i] (and its value) when it is a capture option. */
int psx_capture_parse_arg(int argc, char **argv, int *i);

/* Any capture option was given: unattended, unpaced, no host audio/OSD. */
int psx_capture_requested(void);
/* The capture session is live (after psx_capture_begin). */
int psx_capture_active(void);
/* Requested drawable size, 0x0 when --capture-size was not given. */
void psx_capture_size(int *w, int *h);

/* Call before the first captured frame, after any requested restore. */
void psx_capture_begin(void);
int psx_capture_failed(void);

/* Renderer hook: nonzero when the composed frame should be read back this
 * swap; psx_capture_store_frame() receives top-down RGB (w*h*3 bytes). */
int  psx_capture_wants_readback(void);
void psx_capture_store_frame(const uint8_t *rgb_top_down, int w, int h);

/* Called once at the start of every guest vblank handler. Writes the frame
 * presented by the previous handler (when inside the range) and decides
 * whether the run is over. Returns 1 when the runtime should exit now. */
int psx_capture_vblank(void);

/* Index of the guest frame the current handler will present. */
uint64_t psx_capture_frame_index(void);

/* SPU final mix, 44.1 kHz interleaved stereo s16, in guest-time order. */
void psx_capture_audio(const int16_t *stereo, uint32_t frames);

/* Scripted input for the frame being presented. Returns 1 when an input
 * script is loaded; *p1 / *p2 then hold active-low pad words (0xFFFF idle). */
int psx_capture_input(int *p1, int *p2);

/* --load-state FILE, or NULL. */
const char *psx_capture_load_state_path(void);

/* Flush files, patch the WAV header, print the one-line summary. Idempotent. */
void psx_capture_finish(void);

#ifdef __cplusplus
}
#endif

#endif
