#ifndef PSX_TAS_CHECKPOINT_H
#define PSX_TAS_CHECKPOINT_H

/*
 * Checkpoint capture and resume for replays: a debug feature, off unless its
 * environment variables are set, and absent from PSX_NO_DEBUG_TOOLS builds.
 *
 *   PSX_TAS_SAVE_STATE_AT=<frame>[,<frame>...]  capture at these frames
 *   PSX_TAS_SAVE_STATE_PATH=<file.pst>          where; "{frame}" in the name
 *                                               becomes the six-digit frame
 *   PSX_TAS_RESUME_STATE=<file.pst>             resume from this state at start
 *   PSX_TAS_RESUME_MANIFEST=<file.json>         its manifest (default: state + ".json")
 *   PSX_TAS_RESUME_COMPATIBLE_BUILD=1           accept a rebuilt executable whose
 *                                               checkpoint compatibility id matches
 *   PSX_TAS_PERTURB_RESTORE=<field>             after an accepted resume, change one
 *                                               restored value (ram, irq_timing,
 *                                               sched, timer2, memctrl) so a comparison of
 *                                               the two runs can be shown to fail
 *
 * Frames are the debug server's frame counter (one per VBlank present). A
 * capture requested at a frame is taken at the next savestate boundary with a
 * safe resume PC; its manifest names both that frame and the requested one.
 * The state, its manifest (source_tas_stateio.h) and one image per inserted
 * memory card (<state>.cardN.mcd) are written side by side; an existing
 * manifest is never overwritten. A resume checks the manifest against this
 * run before and after loading and exits with status 2 on any refusal. It
 * never writes the bound memory card files: an inserted card must already
 * hold the checkpoint's image.
 */

#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Once at startup, after savestate_configure. Hashes the executable when a
 * capture or resume is requested, then resumes if PSX_TAS_RESUME_STATE is
 * set. Returns 0 to continue, or the process exit status (2) on a refusal. */
int tas_checkpoint_start(CPUState *cpu);

/* Each presented frame, after the frame counter advances. */
void tas_checkpoint_on_frame(uint64_t frame);

/* The two halves, exposed for tests. capture writes the state, its card
 * images and its manifest; resume returns 1 when the state was loaded and
 * accepted, else 0 with a reason (the state may already be loaded). */
int tas_checkpoint_capture(const CPUState *snap, uint32_t bios_checksum, uint32_t entry_pc,
                           unsigned frame, unsigned requested_frame, const char *path);
int tas_checkpoint_resume(CPUState *cpu, const char *state_path, const char *manifest_path,
                          char *reason, size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif
