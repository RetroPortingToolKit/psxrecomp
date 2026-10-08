"""Build and run test_gl_scale_invariance.c on a hidden real OpenGL context.

The fixture runs once per internal scale; the guest-visible (native) VRAM
digest must be identical at every scale, and each run checks line thickness
at internal resolution. Window runs (PSX_GL_HIRES_WINDOW=1) keep VRAM at 1x
and only the frame at S: their native digest must match too, and their frame
and native-wide (16:9) surface at internal resolution must equal the
full-VRAM run's at the same scale.
An 18x window run is 8K past a 16384 texture limit. Two clamp runs check that
an over-limit request (32x, and a tiny memory budget) stays on GL.
Side-by-side runs (mode sbs) flip two 512-wide buffers at x=0 and x=512: the
window must hold both at S and match the full-VRAM surface, as one surface
while their union fits (5x) and as two tiles when it does not (9x under a
simulated 8192 limit, PSX_GL_MAX_DIM, and 18x on a 16384 GPU); copies 1000 px
wide are staged in chunks the limit allows.
Line runs (mode lines) batch lines with triangles over native-wide: the window
runs' native frame must equal the 1x run's (the 1x authoritative surface draws
its lines as GL_LINES, in painter order), and their frame and wide surface at S
the full-VRAM run's at the same scale.
Capture runs (mode capture) check the frame-blend history and hold-last
captures: at the source scale outside the window mode, at the presented
(letterbox) size in it.
Mask runs (mode mask) change the GP0(E6) mask-check bit after a line, a flat
triangle or an opaque textured rect and before its batch is drawn: the draw
must keep the check bit it was submitted under, at 1x, above it and in the
window mode.
Texture-window runs (mode twin) draw prims that alternate GP0(E2h) windows
with [video] texture_window_batching off and on: the native VRAM, the frame at
S and the wide surface must be the same both ways, at 1x, above it and in the
window mode, and on must draw in fewer batches. Each of those runs also checks
raw rects drawn through every window against the PS1 window rule computed in C
from the VRAM, so a decode error both ways share still fails.
Pass runs (mode passes, where the renderer has the frame-rate stack's render
passes) check that the window mode refuses them and the full-VRAM surface
offers them.
Step runs (mode steps, dynamic resolution) keep the surfaces at a ceiling and
change the scale at run time. "fresh" steps before drawing: the native VRAM,
the frame at S and the wide surface must equal the fixed-scale run at the
final level. "chain" draws, then steps through a list: each step must leave
the native VRAM unchanged and rescale the displayed rect, the draw area and
the wide margins from their own pixels (the fixture checks every pixel); the
scene drawn again at the final level must equal the fixed-scale run there.
The window mode refuses steps.

macOS/Linux/Windows (MinGW): pass the SDL3 include directory and static
library (for example from a runtime build tree's _deps/sdl3-src/include and
_deps/sdl3-build/libSDL3.a) and a C compiler. Evidence (commands, output) is
written to receipt.json under --output.
When the host cannot create a hidden window with a GL 3.3 core context (a
headless or non-interactive session, such as Windows over SSH) the fixture
exits SKIP_EXIT and so does this script: CTest reports a skip, not a pass.
"""
import argparse
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile

MAC_FRAMEWORKS = ["Cocoa", "OpenGL", "IOKit", "CoreVideo", "CoreAudio", "AudioToolbox",
                  "Carbon", "ForceFeedback", "GameController", "Metal", "QuartzCore",
                  "CoreMedia", "AVFoundation", "Foundation", "CoreHaptics",
                  "UniformTypeIdentifiers"]
# The static SDL3's Win32 dependencies (as run_gl_texture_filter.py links them).
WIN_LIBS = ["opengl32", "kernel32", "user32", "gdi32", "winmm", "imm32", "ole32",
            "oleaut32", "version", "uuid", "advapi32", "setupapi", "shell32", "dinput8"]
SKIP_EXIT = 77  # CTest SKIP_RETURN_CODE, as wtrace_dump_test uses it
WINDOWS = os.name == "nt" or platform.system().startswith(("MINGW", "MSYS", "CYGWIN"))


def parse_run(stdout):
    """Return (checks, failures, digest or None) from a fixture run."""
    summary = re.search(r"^checks=(\d+) failures=(\d+)$", stdout, re.M)
    digest = re.search(r"^digest=([0-9a-f]{16})$", stdout, re.M)
    if not summary:
        return None
    return int(summary[1]), int(summary[2]), digest[1] if digest else None


def parse_hires(stdout, key="hires"):
    m = re.search(r"^" + key + r"=([0-9a-f]{16})$", stdout, re.M)
    return m[1] if m else None


def digests_agree(results):
    """results: {scale: digest}. All present and equal."""
    values = list(results.values())
    return bool(values) and all(v is not None for v in values) and len(set(values)) == 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--sdl-include", required=True)
    ap.add_argument("--sdl-library", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--fixture", type=pathlib.Path)
    ap.add_argument("--scales", default="1,2,3,5,9")
    ap.add_argument("--window-scales", default="2,3,5,9,18")
    args = ap.parse_args()
    # ';'-separated when CMake hands over a target's include list.
    sdl_includes = [str(pathlib.Path(d).resolve()) for d in args.sdl_include.split(";") if d]
    args.sdl_library = str(pathlib.Path(args.sdl_library).resolve())
    framework = pathlib.Path(__file__).resolve().parents[2]
    fixture = args.fixture or framework / "runtime/tests/test_gl_scale_invariance.c"
    out_root = pathlib.Path(args.output).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    dest = pathlib.Path(tempfile.mkdtemp(prefix="scale-", dir=out_root))
    print("Evidence directory:", dest)
    receipt = []

    def run(command, env=None):
        command = [str(c) for c in command]
        r = subprocess.run(command, cwd=dest, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
        receipt.append({"cmd": command, "exit": r.returncode,
                        "stdout": r.stdout, "stderr": r.stderr[-4000:]})
        (dest / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        return r

    includes = ["-I", framework / "runtime/include", "-I", framework / "runtime/src"]
    for d in sdl_includes:
        includes += ["-I", d]
    # The fixture #includes gpu_gl_renderer.c; these are the other runtime
    # sources the renderer calls into. render_pass_plan.c exists once the
    # frame-rate render passes have landed, and the renderer calls it from then
    # on, so it is linked whenever it is there.
    sources = [("probe", fixture), ("sw", framework / "runtime/src/gpu_sw_renderer.c"),
               ("fi", framework / "runtime/src/frame_interpolation.c")]
    if (framework / "runtime/src/render_pass_plan.c").exists():
        sources.append(("rp", framework / "runtime/src/render_pass_plan.c"))
    if (framework / "runtime/src/psx_openxr.c").exists():
        sources.append(("xr", framework / "runtime/src/psx_openxr.c"))
    if (framework / "runtime/src/render_thread.c").exists():
        sources.append(("rth", framework / "runtime/src/render_thread.c"))
    if (framework / "runtime/src/present_thread.c").exists():
        sources.append(("pth", framework / "runtime/src/present_thread.c"))
    if (framework / "runtime/src/frame_gen.c").exists():
        sources.append(("fg", framework / "runtime/src/frame_gen.c"))
    # Unused renderer functions reference the rest of the runtime; the linker
    # drops them (-dead_strip, or per-function sections with --gc-sections).
    # MinGW's PE linker reports undefined references from sections it later
    # collects, so there LTO drops them first (as run_gl_readback_region.py).
    # Anything still unresolved is a link error, not a NULL call at run time.
    sections = [] if platform.system() == "Darwin" else ["-ffunction-sections", "-fdata-sections"]
    if WINDOWS:
        sections.append("-flto")
    # Render passes (the frame-rate stack): build the fixture's passes mode.
    renderer = (framework / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")
    passes = "uint32_t gl_renderer_pass_unavailable(void)" in renderer
    if passes:
        sections.append("-DPSX_TEST_RENDER_PASSES=1")
    objs = []
    for name, src in sources:
        o = dest / (name + ".o")
        r = run([args.cc, "-std=gnu11", "-O1", "-DPSX_SDL3=1", "-DPSX_NO_DEBUG_TOOLS=1",
                 "-DGL_SILENCE_DEPRECATION=1", "-w", *sections, *includes, "-c", src, "-o", o])
        if r.returncode:
            print(r.stderr[-3000:])
            return 2
        objs.append(o)
    probe = dest / ("probe.exe" if WINDOWS else "probe")
    link = [args.cc, *objs, args.sdl_library, "-o", probe]
    if platform.system() == "Darwin":
        for f in MAC_FRAMEWORKS:
            link += ["-framework", f]
        link += ["-liconv", "-lm", "-Wl,-dead_strip"]
    elif WINDOWS:
        # -static: no libwinpthread/libgcc DLLs needed next to probe.exe.
        link += ["-flto", "-static", *["-l" + x for x in WIN_LIBS], "-lm", "-Wl,--gc-sections"]
    else:
        link += ["-lGL", "-lm", "-ldl", "-lpthread", "-Wl,--gc-sections"]
    r = run(link)
    if r.returncode:
        print(r.stderr[-3000:])
        return 2

    # One preflight run: no window/GL context on this host means skip, before
    # any check is counted. After this, a skip exit is a failure like any other.
    r = run([probe, 1])
    if r.returncode == SKIP_EXIT:
        print("SKIP:", r.stderr.strip()[-600:])
        return SKIP_EXIT

    ok = True
    digests = {}
    hires_full = {}
    for s in [int(v) for v in args.scales.split(",") if v]:
        r = run([probe, s])
        parsed = parse_run(r.stdout)
        print(f"scale {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-3:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        digests[("full", s)] = parsed[2] if parsed else None
        hires_full[s] = (parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
    env = os.environ.copy()
    wenv = dict(env)
    wenv["PSX_GL_HIRES_WINDOW"] = "1"
    for s in [int(v) for v in args.window_scales.split(",") if v]:
        r = run([probe, s, "window"], env=wenv)
        parsed = parse_run(r.stdout)
        print(f"window {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-3:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        digests[("window", s)] = parsed[2] if parsed else None
        h = (parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
        if s in hires_full and hires_full[s] != h:
            print(f"FAIL window {s}x frame/wide surface differ from the full-VRAM "
                  f"surface at {s}x:", h, hires_full[s])
            ok = False
    if not digests_agree(digests):
        print("FAIL native VRAM digest differs across scales/modes:", digests)
        ok = False
    # Side-by-side flips: (label, scale, env, reference run or None, tiles).
    sbs = {}
    sbs_runs = (
        ("full", 1, {}, None, None),
        ("full", 5, {}, None, None),
        ("full", 9, {}, None, None),
        ("window", 5, {"PSX_GL_HIRES_WINDOW": "1"}, ("full", 5), 1),
        ("window-8192", 9, {"PSX_GL_MAX_DIM": "8192"}, ("full", 9), 2),
        ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"}, None, None),
    )
    for label, s, extra, ref, tiles in sbs_runs:
        e = dict(env)
        e.update(extra)
        r = run([probe, s, "sbs"], env=e)
        parsed = parse_run(r.stdout)
        got_tiles = re.search(r"^tiles=(\d+)$", r.stdout, re.M)
        print(f"sbs {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-5:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        sbs[(label, s)] = (parsed[2] if parsed else None,
                           parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
        if tiles is not None and (not got_tiles or int(got_tiles[1]) != tiles):
            print(f"FAIL sbs {label} {s}x: tiles", got_tiles and got_tiles[1], "want", tiles)
            ok = False
        if ref is not None and sbs.get(ref, (None,))[1:] != sbs[(label, s)][1:]:
            print(f"FAIL sbs {label} {s}x buffers differ from {ref}:",
                  sbs[(label, s)][1:], sbs.get(ref))
            ok = False
    if not digests_agree({k: v[0] for k, v in sbs.items()}):
        print("FAIL sbs native VRAM digest differs across scales/modes:", sbs)
        ok = False
    # Lines batched with triangles: (label, scale, env).
    lines = {}
    for label, s, extra in (("full", 1, {}), ("full", 9, {}),
                            ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}),
                            ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"})):
        e = dict(env)
        e.update(extra)
        r = run([probe, s, "lines"], env=e)
        parsed = parse_run(r.stdout)
        print(f"lines {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-4:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        lines[(label, s)] = (parse_hires(r.stdout, "lband"), parse_hires(r.stdout),
                             parse_hires(r.stdout, "wide"))
    for key in (("window", 9), ("window", 18)):
        if lines[key][0] is None or lines[key][0] != lines[("full", 1)][0]:
            print(f"FAIL lines {key}: native frame differs from the 1x run:",
                  lines[key][0], lines[("full", 1)][0])
            ok = False
    if None in lines[("window", 9)][1:] or lines[("window", 9)][1:] != lines[("full", 9)][1:]:
        print("FAIL lines window 9x frame/wide surface differ from the full-VRAM run:",
              lines[("window", 9)][1:], lines[("full", 9)][1:])
        ok = False
    # Frame-blend and hold-last captures: (label, scale, env, windowed).
    for label, s, extra, windowed in (("full", 9, {}, False),
                                      ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}, True),
                                      ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"}, True)):
        e = dict(env)
        e.update(extra)
        r = run([probe, s, "capture"], env=e)
        parsed = parse_run(r.stdout)
        print(f"capture {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-2:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        m = re.search(r"^capture=(\d+)x(\d+) source=(\d+)x(\d+)$", r.stdout, re.M)
        if not m or ((m[1], m[2]) == (m[3], m[4])) == windowed:
            print(f"FAIL capture {label} {s}x: size", m and m.group(0))
            ok = False
    # Mask-check changes between a draw and its batch: (label, scale, env).
    for label, s, extra in (("full", 1, {}), ("full", 4, {}), ("full", 9, {}),
                            ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}),
                            ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"})):
        e = dict(env)
        e.update(extra)
        r = run([probe, s, "mask"], env=e)
        parsed = parse_run(r.stdout)
        print(f"mask {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-1:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
    # Texture-window batching off vs on: (label, scale, env).
    for label, s, extra in (("full", 1, {}), ("full", 4, {}), ("full", 9, {}),
                            ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}),
                            ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"})):
        e = dict(env)
        e.update(extra)
        got = {}
        for on in (0, 1):
            r = run([probe, s, "twin", on], env=e)
            parsed = parse_run(r.stdout)
            m = re.search(r"^twin_flushes=(\d+) batches=(\d+)$", r.stdout, re.M)
            print(f"twin {label} {s} batching={on}: exit={r.returncode}",
                  r.stdout.strip().splitlines()[-5:], r.stderr.strip()[-600:])
            if r.returncode or not parsed or parsed[1] or not m:
                ok = False
            got[on] = (parsed[2] if parsed else None, parse_hires(r.stdout),
                       parse_hires(r.stdout, "wide"), int(m[2]) if m else None)
        if None in got[0][:3] or got[0][:3] != got[1][:3]:
            print(f"FAIL twin {label} {s}x: batching changes the image:", got)
            ok = False
        if got[0][3] is None or got[1][3] is None or got[1][3] >= got[0][3]:
            print(f"FAIL twin {label} {s}x: batching on did not draw fewer batches:", got)
            ok = False
    # Native-wide mirror queue and stale-rect wide stencil rebuild: the same
    # native VRAM, frame at S and wide surface with each off (the previous
    # immediate mirrors and whole-surface rebuilds) and on.
    ab_envs = (("default", {}), ("queue-off", {"PSX_GL_WIDE_QUEUE": "0"}),
               ("stencil-full", {"PSX_GL_WIDE_STENCIL_FULL": "1"}),
               ("both-off", {"PSX_GL_WIDE_QUEUE": "0", "PSX_GL_WIDE_STENCIL_FULL": "1"}))
    ab_runs = [("wmask", s, fast, {}) for s in (1, 3, 9) for fast in ("0", "1")]
    ab_runs += [("wmask", 9, fast, {"PSX_GL_HIRES_WINDOW": "1"}) for fast in ("0", "1")]
    ab_runs += [("lines", 9, None, {}), ("twin", 9, "1", {}), ("scene", 3, None, {}),
                ("scene", 9, None, {})]
    for mode, s, arg, extra in ab_runs:
        got = {}
        for name, ab in ab_envs:
            e = dict(env)
            e.update(extra)
            e.update(ab)
            cmd = [dest / "probe", s, mode] + ([arg] if arg is not None else [])
            r = run(cmd, env=e)
            parsed = parse_run(r.stdout)
            if r.returncode or not parsed or parsed[1]:
                print(f"wide a/b {mode} {s} {arg} {name}: exit={r.returncode}",
                      r.stdout.strip().splitlines()[-3:], r.stderr.strip()[-600:])
                ok = False
            got[name] = (parsed[2] if parsed else None, parse_hires(r.stdout),
                         parse_hires(r.stdout, "wide"))
        same = len(set(got.values())) == 1 and None not in got["default"][1:]
        print(f"wide a/b {mode} {s}x {arg or ''}{' window' if extra else ''}:",
              "same" if same else got)
        if not same:
            print(f"FAIL wide a/b {mode} {s}x: queue/stencil change the image:", got)
            ok = False
    # Render passes: offered on the full-VRAM surface, refused in the window mode.
    if not passes:
        print("passes: skipped (this renderer has no render passes)")
    for label, s, extra in ((("full", 9, {}), ("window", 9, {"PSX_GL_HIRES_WINDOW": "1"}),
                             ("window", 18, {"PSX_GL_HIRES_WINDOW": "1"})) if passes else ()):
        e = dict(env)
        e.update(extra)
        r = run([probe, s, "passes"], env=e)
        parsed = parse_run(r.stdout)
        print(f"passes {label} {s}: exit={r.returncode}", r.stdout.strip().splitlines()[-2:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
    # Dynamic resolution steps: (ceiling, levels, kind, env). The final level
    # must be one of --scales (its fixed-scale run is the reference).
    step_runs = (
        (9, "5", "fresh", {}), (9, "3,9", "fresh", {}), (9, "1", "fresh", {}),
        (5, "2", "fresh", {}),
        (9, "8,5,9,3,1,2,9", "chain", {}), (5, "3,5,1,2", "chain", {}),
        (9, "5", "chain", {"PSX_GL_HIRES_WINDOW": "1"}),
    )
    for ceiling, levels, kind, extra in step_runs:
        e = dict(env)
        e.update(extra)
        r = run([dest / "probe", ceiling, "steps", levels, kind], env=e)
        parsed = parse_run(r.stdout)
        label = f"steps {kind} {ceiling}x [{levels}]" + (" window" if extra else "")
        print(f"{label}: exit={r.returncode}",
              [ln for ln in r.stdout.strip().splitlines() if ln.startswith("step ")][-8:],
              r.stdout.strip().splitlines()[-1:], r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
        if extra:
            continue   # the window mode only has to refuse
        final = int(levels.split(",")[-1])
        if parsed:
            digests[("steps", ceiling, levels, kind)] = parsed[2]
        got = (parse_hires(r.stdout), parse_hires(r.stdout, "wide"))
        if final not in hires_full or None in got or got != hires_full[final]:
            print(f"FAIL {label}: frame/wide surface differ from the fixed {final}x run:",
                  got, hires_full.get(final))
            ok = False
    if not digests_agree(digests):
        print("FAIL native VRAM digest differs across steps:", digests)
        ok = False
    for label, s, budget in (("over-limit", 32, None), ("budget", 12, "40")):
        e = dict(env)
        if budget is not None:
            e["PSX_GL_VRAM_BUDGET_MB"] = budget
        r = run([probe, s, "clamp"], env=e)
        parsed = parse_run(r.stdout)
        print(f"clamp {label}: exit={r.returncode}", r.stdout.strip().splitlines()[-2:],
              r.stderr.strip()[-600:])
        if r.returncode or not parsed or parsed[1]:
            ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
