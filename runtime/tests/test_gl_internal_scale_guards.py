"""Source guards for the OpenGL internal-resolution scale (source-only).

The real-GL behaviour is covered by run_gl_scale_invariance.py (needs a GPU and
SDL3). These guards pin the structural promises that keep native (1x) output
byte-identical and keep a large scale from costing the GL backend:
  - no local 4x cap in the GL backend; the ceiling lives in gpu_render.h;
  - init queries the driver limits and clamps, and only a 1x allocation
    failure can drop the backend to software;
  - line quads, the area resolve and the half-texel inset change are all
    gated on a scale above 1;
  - the dual-raster re-arm and the offline clamp no longer cap GL at the
    software mirror's 4x.
Also unit-tests the invariance runner's result parsing.
"""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GL = (ROOT / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
RENDER_H = (ROOT / "runtime/include/gpu_render.h").read_text(encoding="utf-8")

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from run_gl_scale_invariance import digests_agree, parse_run  # noqa: E402


def body(src, signature):
    # The definition, not a prototype: the first occurrence whose next '{'
    # comes before any ';'.
    start = src.index(signature)
    while True:
        brace, semi = src.find("{", start), src.find(";", start)
        if brace != -1 and (semi == -1 or brace < semi):
            break
        start = src.index(signature, start + 1)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
    raise AssertionError("unbalanced " + signature)


class GlScaleGuards(unittest.TestCase):
    def test_ceiling_is_shared(self):
        self.assertIsNone(re.search(r"#define\s+GL_MAX_INTERNAL_SCALE\s+4\b", GL))
        self.assertRegex(RENDER_H, r"#define\s+GL_MAX_INTERNAL_SCALE\s+32\b")

    def test_init_clamps_to_driver_limits(self):
        init = body(GL, "static int init_gpu_raster(void)")
        for token in ("GL_MAX_TEXTURE_SIZE", "PSXGL_MAX_RENDERBUFFER_SIZE",
                      "GL_MAX_VIEWPORT_DIMS", "psx_gl_clamp_full_vram_scale"):
            self.assertIn(token, init)
        # Retry lower inside GL; only a 1x failure returns 0 (software fallback).
        self.assertRegex(init, r"while \(!alloc_hr_targets\(s_hr_scale\)\) \{\s*if \(s_hr_scale <= 1\) return 0;")

    def test_s1_paths_unchanged(self):
        geo = body(GL, "static void gpu_geometry(")
        self.assertIn("if (is_line && s_hr_scale > 1) {", geo)
        self.assertIn("if (draw_mode == GL_LINES) glLineWidth((float)s_hr_scale);", geo)
        quad = body(GL, "static void present_target_quad(GLuint tex, float tex_w, float tex_h,\n"
                        "                                int x, int y, int w, int h, int linear,\n"
                        "                                int lx, int ly, int lw, int lh, int v_flip,\n"
                        "                                int apply_gamma, int src_scale) {")
        self.assertIn("if (src_scale > 1 && lw > 0", quad)
        self.assertIn("float in = src_scale > 1 ? 0.5f / (float)src_scale : 0.5f;", quad)
        stencil = body(GL, "static void rebuild_mask_stencils(void)")
        self.assertIn("if (s_out_scale <= 1) {", stencil)

    def test_lines_batch_above_1x(self):
        # Above 1x a line quad joins the flat batch (one draw, one wide mirror
        # per batch instead of two surface switches per line), windowed mode
        # included; 1x and backdrop-stretched lines keep the immediate path.
        # The invariance runner's line bands and lines runs prove the pixels
        # are unchanged.
        geo = body(GL, "static void gpu_geometry(")
        self.assertIn("if (mode == GL_LINES && n == 2 && s_out_scale > 1 && "
                      "!bd_prim_gate(xs, n, 0)) {", geo)
        batched = geo[geo.index("line_to_quad(lv, quad);"):]
        batched = batched[:batched.index("return;")]
        self.assertIn("s_fb_n += 6;", batched)
        self.assertNotIn("glDrawArrays", batched)
        # Windowed: the 1x hr surface keeps the line's GL_LINES vertices.
        self.assertIn("if (s_hiw) {", batched)
        self.assertIn("s_fbl_at[s_fbl_n] = s_fb_n;", batched)

    def test_windowed_lines_two_vertex_sets(self):
        # A batch with windowed lines draws on the hr surface (1x in windowed
        # mode) through flat_batch_draw_hr_lines, lines as GL_LINES from the
        # copy uploaded after the batch; any other batch keeps its one draw.
        # The window and the wide surface draw only the batch's nverts
        # vertices (triangles and line quads). (A GL_LINES batch at 1x, where
        # one exists, draws with its own mode in both places.)
        flush = body(GL, "static void flush_flat_batch(void)")
        self.assertIn("s_fbl_n = 0;", flush)
        self.assertIn("memcpy(&s_fb[nverts * 6], s_fbl,", flush)
        self.assertIn("(nverts + 2 * nl) * 6 * sizeof(float)", flush)
        self.assertRegex(flush, r"if \(nl\) flat_batch_draw_hr_lines\(nverts, nl\);\n"
                                r"\s*else glDrawArrays\((GL_TRIANGLES|fmode), 0, nverts\);")
        self.assertIn("hiw_enqueue_geo(s_fb, nverts, semi, mask, mirror, s_fb_gate)", flush)
        wide = flush[flush.index("wide_target_begin("):]
        self.assertRegex(wide, r"glDrawArrays\((GL_TRIANGLES|fmode), 0, nverts\);")
        hr = body(GL, "static void flat_batch_draw_hr_lines(int nverts, int nl)")
        self.assertIn("glDrawArrays(GL_LINES, nverts + 2 * i, 2 * (j - i));", hr)
        # Room for the copy: every line takes six batch vertices.
        self.assertIn("#define FLATBATCH_MAXL (FLATBATCH_MAXV / 6)", GL)
        self.assertIn("static float s_fb[(FLATBATCH_MAXV + 2 * FLATBATCH_MAXL) * 6];", GL)

    def test_main_does_not_cap_gl_at_software_limit(self):
        self.assertNotIn("if (want > SW_MAX_INTERNAL_SCALE) want = SW_MAX_INTERNAL_SCALE;", MAIN)
        self.assertIn("(g_video_renderer == 1) ? GL_MAX_INTERNAL_SCALE", MAIN)


class HiresWindowGuards(unittest.TestCase):
    """Windowed high-resolution mode (true 8K past a 16384 texture limit):
    engaged only beyond the full-VRAM clamp (or forced for tests), and every
    mirror is a no-op unless it is."""

    def test_engaged_only_past_full_vram(self):
        init = body(GL, "static int init_gpu_raster(void)")
        self.assertIn("if (allow && want > 1 && (force || want > s_out_scale)) {", init)
        self.assertIn("s_hr_scale = 1;", init)

    def test_mirrors_gated(self):
        for fn in ("static void hiw_mirror_uploads(", "static void hiw_mirror_copy(",
                   "static void hiw_clear_rect("):
            self.assertIn("if (!hiw_on()", body(GL, fn))
            # an immediate write into the window lands after every queued draw
            self.assertIn("hiw_flush_queue();", body(GL, fn))
        # Queued when the window is on, or (full-VRAM surface) when a native-wide
        # mirror can ride the queue (wide_queue_live).
        for site in ("flush_tex_batch(void)", "flush_flat_batch(void)"):
            self.assertRegex(body(GL, "static void " + site),
                             r"if \(\(hiw_on\(\) \|\| \(mirror[^)]*wide_queue_live\(\)\)\) &&\s*hiw_enqueue_")

    def test_wide_mirror_queued_in_windowed_mode(self):
        # Windowed mode: a draw's native-wide mirror rides in its window queue
        # entry and is replayed in the queue's flush (one pass per wide
        # surface), not as a surface switch per batch. The immediate mirror
        # runs only when the entry did not take it.
        for site, call in (("static void flush_tex_batch(void)",
                            "hiw_enqueue_tex(nverts, semi, mirror, s_tb_gate)) mirror = 0;"),
                           ("static void flush_flat_batch(void)",
                            "hiw_enqueue_geo(s_fb, nverts, semi, mask, mirror, s_fb_gate)) mirror = 0;"),
                           ("static void gpu_geometry(", "mirror = 0;")):
            fn = body(GL, site)
            self.assertIn(call, fn, site)
            # the immediate mirror is the last `if (mirror) {` (an earlier one
            # notes the stencil the draw leaves behind)
            self.assertLess(fn.index(call), fn.rindex("if (mirror) {"), site)
            self.assertIn("wide_target_begin(", fn[fn.rindex("if (mirror) {"):], site)
        ok = body(GL, "static int hiw_wide_queue_ok(int mirror)")
        self.assertIn("return mirror && g_wide_cur && s_ws_ablate == 0;", ok)
        for fn in ("static int hiw_enqueue_tex(", "static int hiw_enqueue_geo("):
            b = body(GL, fn)
            self.assertIn("if (!wq && !hiw_area_touches()) return 0;", b)
            self.assertIn("if (wq) hiw_wide_set(c, gate);", b)
        flush = body(GL, "static void hiw_flush_queue(void)")
        if "hiw_flush_tail();" in flush:   # shared with the native-wide-only queue
            flush = body(GL, "static void hiw_flush_tail(void) {")
        self.assertIn("hiw_replay_wide();", flush)
        self.assertLess(flush.index("hiw_replay_wide();"), flush.index("hr_end();"))
        replay = body(GL, "static void hiw_replay_wide(void)")
        for need in ("if (!c->wfbo) continue;", "p_glBindFramebuffer(PSXGL_FRAMEBUFFER, c->wfbo);",
                     "glViewport(0, 0, g_wide_w * S, VRAM_H * S);",
                     "glScissor(c->wsx * S, c->wsy * S, c->wsw * S, c->wsh * S);",
                     "p_glUniform1f(s_tex_uXoff, (float)c->wdx);",
                     "p_glUniform1f(s_geo_uXoff, (float)c->wdx);",
                     "tex_draw_passes_ex(c->vcount, c->semi, c->mask, c->check, 0);",
                     "mask_stencil_ex(c->mask, c->check);"):
            self.assertIn(need, replay)
        # Every other write to a wide surface, and every read of one, lands
        # after the queued mirrors.
        overlay = body(GL, "static void gpu_flat_rect(")
        self.assertLess(overlay.index("hiw_flush_queue();"),
                        overlay.index("wide_flat_rect_direct(0, y, g_wide_w, h, c, semi);"))
        for fn in ("static void glb_wide_configure(",
                   "static void glb_wide_clear(", "static void glb_wide_clear_margins(",
                   "static int glb_render_wide_display(", "static int glb_wide_dump_full(",
                   "static int present_wide_fbo_impl(", "static void rebuild_mask_stencils(void)"):
            self.assertIn("hiw_flush_queue();", body(GL, fn), fn)

    def test_queue_syncs_before_the_raw_mirror_changes(self):
        # Queued window draws sample the raw mirror; it must not change under them.
        self.assertIn("hiw_flush_queue();", body(GL, "static void pack_flush(void)"))
        upload = body(GL, "static void flush_cpu_upload(void)")
        self.assertLess(upload.index("hiw_flush_queue();"), upload.index("glTexSubImage2D"))
        self.assertIn("hiw_flush_queue();", body(GL, "static void depth24_clear_skipped_fb(void)"))
        self.assertIn("hiw_flush_queue();", body(GL, "static void rebuild_mask_stencils(void)"))
        self.assertIn("hiw_flush_queue();", body(GL, "static const HiwTile *hiw_ensure(int x0, int x1)"))
        present = body(GL, "static void present_vram_impl(int disp_x, int disp_y, int w, int h, int linear,")
        self.assertIn("if (s_hiw) {", present)
        self.assertIn("int src_tw = VRAM_W, src_x = disp_x, src_scale = s_out_scale;", present)

    def test_every_tile_is_written(self):
        # Side-by-side buffers too wide for one surface become tiles; every
        # mirror writes each tile it touches, and a display the union cannot
        # hold gets a tile of its own instead of presenting at 1x.
        for fn in ("static void hiw_flush_queue(void)", "static void hiw_clear_rect(",
                   "static void hiw_mirror_uploads(", "static void hiw_mirror_copy_chunk(",
                   "static void rebuild_mask_stencils(void)"):
            self.assertIn("for (int t = 0; ", body(GL, fn), fn)
        ensure = body(GL, "static const HiwTile *hiw_ensure(int x0, int x1)")
        self.assertIn("T = hiw_alloc_tile(u0, u1, all);", ensure)
        self.assertIn("T = hiw_alloc_tile(a0, a1, 0);", ensure)

    def test_window_copy_staging_fits_the_limit(self):
        # The window's S-scaled copy source has its own scratch, staged in
        # column chunks that fit the GPU limit; the shared scratch never grows
        # past the hr surface for it.
        copy = body(GL, "static void gpu_copy_rect(int sx,int sy,int dx,int dy,int w,int h)")
        self.assertNotIn("s_out_scale", copy)
        self.assertIn("if (!scratch_ensure(w * S, h * S)) return;", copy)
        self.assertIn("int cols = s_gl_max_dim > 0 ? s_gl_max_dim / S : hi - lo;",
                      body(GL, "static void hiw_mirror_copy("))

    def test_staging_size_committed_after_allocation(self):
        grow = body(GL, "static int stage_tex_grow(")
        self.assertIn("nw > s_gl_max_dim || nh > s_gl_max_dim", grow)
        self.assertLess(grow.index("glTexImage2D"), grow.index("*cur_w = nw; *cur_h = nh;"))
        self.assertLess(grow.index("glGetError() != GL_NO_ERROR) {"),
                        grow.index("*cur_w = nw; *cur_h = nh;"))
        self.assertIn("stage_tex_grow(s_scratch_tex, &s_scratch_w, &s_scratch_h",
                      body(GL, "static int scratch_ensure(int w, int h)"))

    def test_canonical_shaders_untouched(self):
        # The window has its own blit program; BLIT_VS keeps the fixed 1024x512
        # projection the canonical path always used.
        self.assertIn('"  gl_Position = vec4((a_pos.x+u_shift)/512.0 - 1.0, (a_pos.y+u_shift)/256.0 - 1.0, 0.0, 1.0); }\\n";', GL)


def functions(src):
    """(name, body) for every top-level function definition."""
    out = []
    for m in re.finditer(r"\n((?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)"
                         r"\s*\([^;{]*\))\s*\{", src):
        start, depth = m.end() - 1, 0
        for i in range(start, len(src)):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    out.append((m.group(2), src[start:i + 1]))
                    break
    return out


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


class ScaleContractGuards(unittest.TestCase):
    """The GL backend has two scales: s_hr_scale (s_hr_fbo, the authoritative
    VRAM surface) and s_out_scale (what is presented: the high-resolution
    window, the wide surfaces, captures). They differ only in windowed
    high-resolution mode, so a mix-up is invisible at every other scale."""

    def test_single_scale_name_is_poisoned(self):
        # Code written against the old single s_scale must not build.
        self.assertRegex(GL, r"#if defined\(__GNUC__\) \|\| defined\(__clang__\)\s*"
                             r"#pragma GCC poison s_scale\s*#endif")
        code = strip_comments(GL).replace("#pragma GCC poison s_scale", "")
        self.assertIsNone(re.search(r"\bs_scale\b", code))

    def test_hr_surface_users_pick_the_right_scale(self):
        # A function that binds s_hr_fbo and uses the presented scale must also
        # handle the window (s_hiw / hiw_on): otherwise it reads or writes hr
        # at the wrong scale in windowed mode.
        code = strip_comments(GL)
        for name, fn in functions(code):
            if not re.search(r"FRAMEBUFFER,\s*s_hr_fbo\b", fn):
                continue
            if "s_out_scale" in fn:
                self.assertTrue("s_hiw" in fn or "hiw_on()" in fn,
                                name + " binds s_hr_fbo at s_out_scale without handling the window")


class RenderPassGuards(unittest.TestCase):
    """Render passes (the frame-rate stack) back up and restore only
    s_hr_fbo, at s_hr_scale. In the window mode the presented surfaces are the
    tiles at s_out_scale, which a pass would not restore, and the queued wide
    mirror replay has no pass clamp: the backend must refuse passes there.
    Skipped where the renderer has no render passes; the GL fixture's passes
    runs check the same refusal on a real context."""

    def test_window_mode_refuses_passes(self):
        if "uint32_t gl_renderer_pass_unavailable(void)" not in GL:
            self.skipTest("no render passes in this tree")
        refuse = body(GL, "uint32_t gl_renderer_pass_unavailable(void)")
        self.assertRegex(strip_comments(refuse),
                         r"if \([^;{}]*\bs_hiw\b[^;{}]*\)\s*return PSX_MOD_RENDER_PASS_BACKEND;")
        self.assertIn("gl_renderer_pass_ready()", body(GL, "uint32_t gl_renderer_pass_plan("))
        begin = body(GL, "int gl_renderer_pass_begin(")
        if "transaction_begin(" in begin:
            self.assertIn("period, reuse, 0)", begin)
            begin = body(GL, "static int transaction_begin(")
        status = "s_pass_begin_diag.status"
        self.assertIn("gl_renderer_pass_unavailable()", begin)
        self.assertRegex(begin, r"if \(s_pass_begin_diag\.status != PSX_MOD_RENDER_PASS_READY\)\s*"
                                r'return pass_begin_refuse\("gl_status"\);')
        self.assertLess(begin.index(status + " !="), begin.index("flush_flat_batch()"))
        self.assertIn("gl_renderer_pass_unavailable() == PSX_MOD_RENDER_PASS_READY",
                      body(GL, "int gl_renderer_pass_ready(void)"))


class InternalResolutionGuards(unittest.TestCase):
    def test_hidpi_window_only_when_opted_in(self):
        self.assertIn("win_flags |= PSX_SDL_WINDOW_HIGH_DENSITY;", MAIN)
        self.assertRegex(MAIN, r"g_video_hidpi_window = g_video_scale_applies &&\s*"
                               r"\(g_video_requested_scale > 1 \|\|\s*"
                               r"effective_internal_resolution\(\) == PSX_IR_DISPLAY\);")
        self.assertRegex(MAIN, r"if \(g_video_hidpi_window\)\s*win_flags \|= PSX_SDL_WINDOW_HIGH_DENSITY;")

    def test_vocabulary_is_optional_abi(self):
        # Builds against an older recomp-ui must still compile: every use of
        # the new launcher fields sits behind the capability macro.
        for field in ("gi->internal_resolution_labels", "ls.internal_resolution",
                      "= internal_resolution_for_launcher();"):
            for m in re.finditer(re.escape(field), MAIN):
                before = MAIN[:m.start()]
                opened = before.count("#if defined(RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION)")
                closed = len(re.findall(r"#endif", before[before.rfind(
                    "#if defined(RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION)"):]))
                self.assertGreater(opened, 0, field)
                self.assertEqual(closed, 0, field + " outside its #if block")

    def test_unset_preset_leaves_supersampling(self):
        self.assertIn("if (preset == PSX_IR_UNSET) return;",
                      body(MAIN, "static void apply_internal_resolution(int display_px_h)"))

    def test_launcher_trips_use_the_round_trip_helpers(self):
        # Both launcher exits (first boot, netplay soft-return) seed and adopt
        # through internal_resolution.h, whose behaviour with and without the
        # Internal resolution row is unit-tested (internal_resolution_test).
        # A bare factor copy next to a sticky preset let the preset override
        # a pick in an older launcher's Supersampling row.
        # A third seed: a graphics preset picked in the launcher fills the
        # rows through the same helper (quality_launcher_apply,
        # docs/QUALITY_PRESETS.md).
        self.assertEqual(MAIN.count("psx_ir_launcher_seed_supersampling("), 3)
        self.assertEqual(MAIN.count("psx_ir_adopt_launcher("), 2)
        self.assertEqual(MAIN.count("kLauncherHasInternalResolution, ir_preset_seeded, ir_ss_seeded,"), 2)
        self.assertNotRegex(MAIN, r"g_video_scale\s*=\s*(seed|ls)\.supersampling;")
        self.assertIn("g_video_internal_res = ir.preset;", MAIN)

    def test_env_override_is_never_persisted(self):
        # PSX_INTERNAL_RESOLUTION wins for the run only: the launcher shows and
        # settings.toml saves the configured preset.
        self.assertIn("if (psx_ir_parse(e, &v)) g_video_internal_res_env = v;", MAIN)
        self.assertNotRegex(MAIN, r"g_video_internal_res\s*=\s*v;")
        self.assertIn("g_video_internal_res_env != PSX_IR_UNSET ? g_video_internal_res_env",
                      body(MAIN, "static int effective_internal_resolution(void)"))


class RunnerParsing(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(parse_run("driver=x\ndigest=0123456789abcdef\nchecks=7 failures=0\n"),
                         (7, 0, "0123456789abcdef"))
        self.assertIsNone(parse_run("garbage"))

    def test_digests(self):
        self.assertTrue(digests_agree({1: "a", 3: "a"}))
        self.assertFalse(digests_agree({1: "a", 3: "b"}))
        self.assertFalse(digests_agree({1: "a", 3: None}))
        self.assertFalse(digests_agree({}))


if __name__ == "__main__":
    unittest.main()
