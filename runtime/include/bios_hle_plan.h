/* bios_hle_plan.h — the ONE place the BIOS-backend axes are decided.
 *
 * The HLE tier has two INDEPENDENT axes, and conflating them was a real bug:
 *
 *   call_hle  — service implemented kernel calls (B0 events) in HLE instead of
 *               the recompiled BIOS. Requires a supported kernel event ABI.
 *
 *   boot_skip — the one-shot shell intercept that skips the BIOS boot animation
 *               and lets Main() proceed straight to SYSTEM.CNF + game EXE load.
 *               Needs only the per-image `shell_entry_phys` anchor, and works
 *               under pure LLE: nothing is synthesized, the shell call simply
 *               returns immediately, exactly as if the player had let the disc
 *               menu exit on its own.
 *
 * THE BUG THIS MODULE FIXES (2026-07-27). main.cpp used to force the requested
 * `bios_hle` flag to false when it declined kernel-call HLE, then derived
 * boot_skip from the already-mutated flag. This also disabled boot skip on
 * images that did export a shell entry.
 *
 * The rule is now explicit and image-independent: `bios_hle` (or the deprecated
 * `fast_boot` alias) means "skip the BIOS boot and go to the game", on EVERY
 * BIOS that exports a shell entry. Whether the same flag additionally routes
 * kernel calls through HLE depends on the event table ABI.
 *
 * Kept dependency-free (no CPUState, no psx_bios_image, no runtime globals) so
 * the whole decision matrix is unit-tested directly — see
 * runtime/tests/test_bios_hle_plan.c.
 */
#ifndef PSXRECOMP_BIOS_HLE_PLAN_H
#define PSXRECOMP_BIOS_HLE_PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

/* What the config/env asked for, plus what the linked BIOS image can support. */
typedef struct PsxBiosHleRequest {
    /* ---- requested (game.toml [runtime] / settings.toml / env) ---- */
    int bios_hle;    /* [runtime] bios_hle, PSX_BIOS_HLE                     */
    int keep_intro;  /* bios_hle_keep_intro, PSX_BIOS_HLE_KEEP_INTRO         */
    int fast_boot;   /* deprecated alias: boot-skip only, never call-HLE     */
    /* ---- capabilities of the selected backend ---- */
    int have_event_abi;         /* image uses the supported kernel event ABI  */
    int have_shell_entry;       /* image exports the shell entry PC           */
    /* ---- session shape ---- */
    int have_game_entry;        /* a game EXE is loaded (0 = BIOS-only run)   */
} PsxBiosHleRequest;

typedef struct PsxBiosHlePlan {
    int call_hle;   /* pass to psx_bios_hle_configure() as call_hle  */
    int boot_skip;  /* pass to psx_bios_hle_configure() as boot_skip */
    /* Denial flags: the feature was asked for and is being refused because the
     * SELECTED image cannot support it. Purely for the startup banner — a
     * silent downgrade is what made the original bug invisible. */
    int call_hle_denied;
    int boot_skip_denied;
} PsxBiosHlePlan;

/* Pure function: same request in, same plan out, no globals touched. */
PsxBiosHlePlan psx_bios_hle_plan(PsxBiosHleRequest req);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_BIOS_HLE_PLAN_H */
