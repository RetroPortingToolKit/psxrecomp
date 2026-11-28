/* Decision-matrix test for psx_bios_hle_plan (runtime/src/bios_hle_plan.c).
 *
 * The property under test is PARITY: the player-facing "skip the BIOS and go
 * straight to the game" flag must resolve to the same boot behaviour on every
 * linked BIOS image that exports a shell entry — retail SCPH-1001 (which also
 * exports a DeliverEvent anchor, so it gets the kernel-call HLE tier too) and
 * the bundled OpenBIOS (which deliberately does not, so its kernel calls stay
 * LLE). The regression this pins: boot_skip used to be derived from the
 * already-anchor-gated call-HLE decision, so choosing OpenBIOS silently left
 * the player sitting in the boot animation.
 */
#include "bios_hle_plan.h"

#include <stdio.h>

static int s_fails = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_fails++;
    }
}

/* The two images this build actually links. Only the anchors differ. */
#define RETAIL_ANCHORS   /* deliver_event_ret */ 1, /* shell_entry */ 1
#define OPENBIOS_ANCHORS /* deliver_event_ret */ 0, /* shell_entry */ 1

static PsxBiosHleRequest req(int bios_hle, int keep_intro, int fast_boot,
                             int have_der, int have_shell, int have_game) {
    PsxBiosHleRequest r;
    r.bios_hle               = bios_hle;
    r.keep_intro             = keep_intro;
    r.fast_boot              = fast_boot;
    r.have_deliver_event_ret = have_der;
    r.have_shell_entry       = have_shell;
    r.have_game_entry        = have_game;
    return r;
}

int main(void) {
    /* ---- 1. PARITY: bios_hle=true skips the boot on BOTH images ---------- */
    PsxBiosHlePlan retail =
        psx_bios_hle_plan(req(1, 0, 0, RETAIL_ANCHORS, 1));
    PsxBiosHlePlan openbios =
        psx_bios_hle_plan(req(1, 0, 0, OPENBIOS_ANCHORS, 1));

    expect(retail.boot_skip == 1,   "retail: bios_hle=true skips the boot");
    expect(openbios.boot_skip == 1, "OpenBIOS: bios_hle=true skips the boot");
    expect(retail.boot_skip == openbios.boot_skip,
           "boot behaviour is identical across BIOS images (the whole point)");
    expect(openbios.boot_skip_denied == 0,
           "OpenBIOS: a shell-entry image never reports a boot-skip denial");

    /* ...while the kernel-call axis still differs, loudly, as designed. */
    expect(retail.call_hle == 1,        "retail: kernel-call HLE granted");
    expect(retail.call_hle_denied == 0, "retail: no call-HLE denial");
    expect(openbios.call_hle == 0,
           "OpenBIOS: kernel-call HLE refused (no DeliverEvent anchor)");
    expect(openbios.call_hle_denied == 1,
           "OpenBIOS: the call-HLE refusal is reported, not silent");

    /* ---- 2. Opting out still works, identically on both images ----------- */
    for (int i = 0; i < 2; i++) {
        const int der = (i == 0) ? 1 : 0;  /* retail, then OpenBIOS */
        const char *who = (i == 0) ? "retail" : "OpenBIOS";

        PsxBiosHlePlan off = psx_bios_hle_plan(req(0, 0, 0, der, 1, 1));
        expect(off.boot_skip == 0 && off.call_hle == 0,
               "bios_hle=false = pure LLE, real intro");
        expect(off.call_hle_denied == 0 && off.boot_skip_denied == 0,
               "bios_hle=false denies nothing (nothing was asked for)");

        PsxBiosHlePlan keep = psx_bios_hle_plan(req(1, 1, 0, der, 1, 1));
        expect(keep.boot_skip == 0, "keep_intro=true plays the real intro");

        /* The deprecated fast_boot alias is boot-only and independent of the
         * call tier — it must skip the boot even with bios_hle off. */
        PsxBiosHlePlan fb = psx_bios_hle_plan(req(0, 0, 1, der, 1, 1));
        expect(fb.boot_skip == 1, "fast_boot alias skips the boot");
        expect(fb.call_hle == 0,  "fast_boot never enables kernel-call HLE");

        /* keep_intro suppresses the bios_hle-implied skip, not an explicit
         * fast_boot request. */
        PsxBiosHlePlan fb_keep = psx_bios_hle_plan(req(1, 1, 1, der, 1, 1));
        expect(fb_keep.boot_skip == 1,
               "explicit fast_boot wins over keep_intro");
        (void)who;
    }

    /* ---- 3. Structural limits ------------------------------------------- */
    /* No game loaded: the shell IS the product, so there is nothing to skip
     * to, and that is not a denial. */
    PsxBiosHlePlan bios_only = psx_bios_hle_plan(req(1, 0, 0, 1, 1, 0));
    expect(bios_only.boot_skip == 0,
           "BIOS-only run: no game entry, no boot-skip");
    expect(bios_only.boot_skip_denied == 0,
           "BIOS-only run: absent game is not a boot-skip denial");
    expect(bios_only.call_hle == 1,
           "BIOS-only run: kernel-call HLE is unaffected by the game's absence");

    /* An image with no shell entry at all cannot skip, and says so. */
    PsxBiosHlePlan no_shell = psx_bios_hle_plan(req(1, 0, 0, 1, 0, 1));
    expect(no_shell.boot_skip == 0,
           "no shell-entry anchor: boot-skip structurally unavailable");
    expect(no_shell.boot_skip_denied == 1,
           "no shell-entry anchor: the refusal is reported");

    /* Neither axis available: everything off, both refusals reported. */
    PsxBiosHlePlan bare = psx_bios_hle_plan(req(1, 0, 0, 0, 0, 1));
    expect(bare.call_hle == 0 && bare.boot_skip == 0, "bare image: pure LLE");
    expect(bare.call_hle_denied == 1 && bare.boot_skip_denied == 1,
           "bare image: both refusals reported");

    /* ---- 4. Purity: exhaustive sweep, no cross-axis contamination -------- */
    for (int bits = 0; bits < 64; bits++) {
        PsxBiosHleRequest r = req((bits >> 0) & 1, (bits >> 1) & 1,
                                  (bits >> 2) & 1, (bits >> 3) & 1,
                                  (bits >> 4) & 1, (bits >> 5) & 1);
        PsxBiosHlePlan p = psx_bios_hle_plan(r);

        /* boot_skip must NEVER depend on the DeliverEvent anchor: flipping
         * only that bit may change call_hle, never the boot decision. */
        PsxBiosHleRequest flipped = r;
        flipped.have_deliver_event_ret = !r.have_deliver_event_ret;
        PsxBiosHlePlan q = psx_bios_hle_plan(flipped);
        expect(p.boot_skip == q.boot_skip,
               "boot-skip is independent of the DeliverEvent anchor");
        expect(p.boot_skip_denied == q.boot_skip_denied,
               "boot-skip denial is independent of the DeliverEvent anchor");

        /* call_hle must never depend on the shell entry or the game. */
        PsxBiosHleRequest flip2 = r;
        flip2.have_shell_entry = !r.have_shell_entry;
        flip2.have_game_entry  = !r.have_game_entry;
        PsxBiosHlePlan s = psx_bios_hle_plan(flip2);
        expect(p.call_hle == s.call_hle,
               "call-HLE is independent of the shell entry / game presence");

        /* A granted axis is never simultaneously reported as denied. */
        expect(!(p.call_hle && p.call_hle_denied),
               "call-HLE never both granted and denied");
        expect(!(p.boot_skip && p.boot_skip_denied),
               "boot-skip never both granted and denied");
    }

    if (s_fails) {
        fprintf(stderr, "bios_hle_plan_test: %d failure(s)\n", s_fails);
        return 1;
    }
    printf("bios_hle_plan_test: OK\n");
    return 0;
}
