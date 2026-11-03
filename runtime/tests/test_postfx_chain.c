/* Post-effect chain presets (postfx_chain.c): the format, built-in presets,
 * errors and file import, with no GL. */
#include "postfx_chain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", what); }
}

static int parse(const char *text, PfxChain *c, char *err) {
    return pfx_chain_parse(text, "t", NULL, NULL, c, err, 256);
}

int main(void) {
    PfxChain c; char err[256];

    check(pfx_chain_builtin("bloom", &c, err, sizeof err), "bloom preset loads");
    check(c.npasses == 8 && c.uses_mask, "bloom: 8 passes, reads the mask");
    check(c.pass[0].scale == 0.25f && c.pass[0].format == PFX_FMT_RGBA16F, "bloom: quarter-res float bright pass");
    check(c.pass[7].ninputs == 2 && c.pass[7].in[0].from == 6 && c.pass[7].in[1].from == PFX_IN_MASK &&
          c.pass[7].blend == PFX_BLEND_SCREEN, "bloom: composite screen-blends previous under the mask");
    check(c.pass[0].in[0].from == PFX_IN_SOURCE_DOWN, "bloom: bright pass reads the scaled-down image");
    for (int i = 0; i < c.npasses; i++)
        for (int k = 0; k < c.pass[i].ninputs; k++)
            check(c.pass[i].in[k].from != PFX_IN_SOURCE, "bloom: no full-size copy of the image");
    check(c.pass[0].reuse_generated && c.pass[6].reuse_generated && !c.pass[7].reuse_generated,
          "bloom: generated frames reuse bright/blur, run the composite");
    const float k = 1.7f;
    check(pfx_chain_set_uniform(&c, "u_strength", 1, &k) == 1 && c.pass[7].u[0].v[0] == 1.7f, "uniform override");
    pfx_chain_free(&c);
    check(pfx_chain_builtin("fxaa", &c, err, sizeof err) && c.npasses == 1 && !c.uses_mask, "fxaa preset");
    pfx_chain_free(&c);
    check(pfx_chain_builtin("fxaa_hq", &c, err, sizeof err) && c.pass[0].u[0].v[0] == 1.0f, "fxaa_hq preset");
    pfx_chain_free(&c);
    check(!pfx_chain_builtin("nope", &c, err, sizeof err), "unknown preset refused");

    check(parse("pfx 1\n# c\npass a\n shader builtin:blur\n input t source\n scale 0.5\n filter nearest\n"
                " uniform u_dir 1 2\n generated reuse\npass b\n generated reuse\n shader builtin:blur\n input t a\n input m mask\n", &c, err),
          "custom chain parses");
    check(c.pass[0].nearest && c.pass[0].u[0].count == 2 && c.pass[1].in[0].from == 0 &&
          c.pass[1].scale == 1.0f && c.uses_mask && c.pass[0].reuse_generated &&
          !c.pass[1].reuse_generated, "custom chain fields (the window pass always runs)");
    pfx_chain_free(&c);

    static const struct { const char *text, *want; } bad[] = {
        { "pass a\n", "line 1: expected 'pfx 1' first" },
        { "pfx 2\n", "line 1: expected 'pfx 1' first" },
        { "pfx 1\n", "line 1: no passes" },
        { "pfx 1\nshader builtin:blur\n", "line 2: 'shader' outside a pass" },
        { "pfx 1\npass a\n input t previous\n", "line 3: the first pass has no previous" },
        { "pfx 1\npass a\n shader builtin:blur\n input t later\n", "line 4: unknown input 'later'" },
        { "pfx 1\npass a\n scale 9\n", "line 3: scale must be in (0, 4]" },
        { "pfx 1\npass a\n uniform u 1 x\n", "line 3: bad number 'x'" },
        { "pfx 1\npass a\n shader builtin:nope\n", "line 3: unknown built-in shader 'nope'" },
        { "pfx 1\npass a\npass a\n", "line 3: duplicate pass 'a'" },
        { "pfx 1\npass a\n input t source\n", "pass 'a' has no shader" },
        { "pfx 1\npass a\n wobble 1\n", "line 3: unknown directive 'wobble'" },
        { "pfx 1\npass a\n generated maybe\n", "line 3: generated must be reuse or run" },
        { "pfx 1\npass a\n blend add\n", "line 3: blend must be replace or screen" },
        { "pfx 1\npass a\n shader builtin:blur\n blend screen\npass b\n shader builtin:blur\n",
          "pass 'a': only the last pass may blend" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int ok = parse(bad[i].text, &c, err);
        if (ok || strncmp(err, bad[i].want, strlen(bad[i].want))) printf("  got '%s' for case %zu\n", ok ? "(ok)" : err, i);
        check(!ok && !strncmp(err, bad[i].want, strlen(bad[i].want)), "error message");
    }

    /* File import: shaders resolve next to the preset. */
    FILE *f = fopen("pfx_chain_test.glsl", "wb"); fputs("void main(){ o = vec4(1.0); }\n", f); fclose(f);
    f = fopen("pfx_chain_test.pfx", "wb"); fputs("pfx 1\npass p\n shader pfx_chain_test.glsl\n input t source\n", f); fclose(f);
    check(pfx_chain_load_file("pfx_chain_test.pfx", &c, err, sizeof err) &&
          !strcmp(c.pass[0].shader, "void main(){ o = vec4(1.0); }\n"), "file preset and shader import");
    pfx_chain_free(&c);
    check(!pfx_chain_load_file("pfx_missing.pfx", &c, err, sizeof err) && strstr(err, "cannot open"), "missing preset");
    /* A preset written with CRLF line ends (Windows editors) parses the same. */
    check(parse("pfx 1\r\npass a\r\n shader builtin:blur\r\n input t source\r\n scale 0.5\r\n", &c, err) &&
          c.pass[0].in[0].from == PFX_IN_SOURCE, "CRLF preset");
    pfx_chain_free(&c);
    remove("pfx_chain_test.glsl"); remove("pfx_chain_test.pfx");

    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
