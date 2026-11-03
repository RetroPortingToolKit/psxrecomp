"""Build and run test_gl_render_thread.c on a hidden real OpenGL context.

Each scale runs twice, render thread off and on ([video] render_thread). The
values the scripted guest read back (rb), the native VRAM (digest), the frame
at internal resolution (hires) and the native-wide surface (wide) must be the
same both ways, and the threaded run must actually have recorded and replayed
the stream (its own checks). A third run per scale with one frame in flight
(PSX_TEST_RT_FRAMES is not needed: the fixture takes the bound as argv) is not
required; the in-flight bound itself is covered by render_thread_test.

macOS/Linux/Windows (MinGW): pass the SDL3 include directory and static
library and a C compiler, as for run_gl_scale_invariance.py. Evidence
(commands, output) is written to receipt.json under --output. When the host
cannot create a hidden window with a GL 3.3 core context (headless, or Windows
over SSH) the fixture exits SKIP_EXIT and so does this script: CTest reports a
skip, not a pass.
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
SKIP_EXIT = 77
WINDOWS = os.name == "nt" or platform.system().startswith(("MINGW", "MSYS", "CYGWIN"))
KEYS = ("rb", "digest", "hires", "wide")


def parse(stdout):
    out = {}
    for k in KEYS:
        m = re.search(r"^" + k + r"=([0-9a-f]{16})$", stdout, re.M)
        out[k] = m[1] if m else None
    m = re.search(r"^checks=(\d+) failures=(\d+)$", stdout, re.M)
    out["failures"] = int(m[2]) if m else None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--sdl-include", required=True)
    ap.add_argument("--sdl-library", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--fixture", type=pathlib.Path)
    ap.add_argument("--scales", default="1,4")
    ap.add_argument("--frames", default="80")
    ap.add_argument("--single", action="store_true",
                    help="run the fixture once with no arguments; its exit code is the result")
    args = ap.parse_args()
    sdl_includes = [str(pathlib.Path(d).resolve()) for d in args.sdl_include.split(";") if d]
    args.sdl_library = str(pathlib.Path(args.sdl_library).resolve())
    framework = pathlib.Path(__file__).resolve().parents[2]
    fixture = args.fixture or framework / "runtime/tests/test_gl_render_thread.c"
    out_root = pathlib.Path(args.output).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    dest = pathlib.Path(tempfile.mkdtemp(prefix="rth-", dir=out_root))
    print("Evidence directory:", dest)
    receipt = []

    def run(command):
        command = [str(c) for c in command]
        r = subprocess.run(command, cwd=dest, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        receipt.append({"cmd": command, "exit": r.returncode,
                        "stdout": r.stdout, "stderr": r.stderr[-4000:]})
        (dest / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        return r

    includes = ["-I", framework / "runtime/include", "-I", framework / "runtime/src"]
    for d in sdl_includes:
        includes += ["-I", d]
    src = framework / "runtime/src"
    # The fixture #includes gpu_gl_renderer.c and drives it through the real
    # facade (gpu_render.c) and render thread (render_thread.c).
    sources = [("probe", fixture), ("sw", src / "gpu_sw_renderer.c"),
               ("fi", src / "frame_interpolation.c"), ("rp", src / "render_pass_plan.c"),
               ("xr", src / "psx_openxr.c"), ("rth", src / "render_thread.c"), ("pth", src / "present_thread.c"),
               ("facade", src / "gpu_render.c"), ("fg", src / "frame_gen.c")]
    sections = [] if platform.system() == "Darwin" else ["-ffunction-sections", "-fdata-sections"]
    # MinGW's PE linker reports undefined references from sections it later
    # collects, so there LTO drops them first (as run_gl_scale_invariance.py).
    if WINDOWS:
        sections.append("-flto")
    objs = []
    # The renderer calls the in-game overlay hook (host_overlay.c) at present.
    if (src / "host_overlay.c").exists():
        sources.append(("hov", src / "host_overlay.c"))
    for name, path in sources:
        o = dest / (name + ".o")
        r = run([args.cc, "-std=gnu11", "-O1", "-DPSX_SDL3=1", "-DPSX_NO_DEBUG_TOOLS=1",
                 "-DGL_SILENCE_DEPRECATION=1", "-w", *sections, *includes, "-c", path, "-o", o])
        if r.returncode:
            print(r.stderr[-3000:])
            return 2
        objs.append(o)
    probe = dest / ("probe.exe" if WINDOWS else "probe")
    link = [args.cc, *objs, args.sdl_library, "-o", probe]
    if platform.system() == "Darwin":
        for f in MAC_FRAMEWORKS:
            link += ["-framework", f]
        link += ["-liconv", "-lm", "-lpthread", "-Wl,-dead_strip"]
    elif WINDOWS:
        # -static: no libwinpthread/libgcc DLLs needed next to probe.exe.
        link += ["-flto", "-static", *["-l" + x for x in WIN_LIBS], "-lm", "-Wl,--gc-sections"]
    else:
        link += ["-lGL", "-lm", "-ldl", "-lpthread", "-Wl,--gc-sections"]
    r = run(link)
    if r.returncode:
        print(r.stderr[-3000:])
        return 2

    if args.single:
        r = run([probe])
        print(r.stdout[-6000:], r.stderr[-3000:])
        if r.returncode == SKIP_EXIT:
            print("SKIP:", r.stderr.strip()[-600:])
        return r.returncode

    ok = True
    first = True
    for s in [int(v) for v in args.scales.split(",") if v]:
        runs = {}
        for threaded in (0, 1):
            r = run([probe, s, threaded, args.frames])
            if first and r.returncode == SKIP_EXIT:
                # No window/GL context on this host: skip. Only the first
                # run may skip; after it, 77 is a failure like any other.
                print("SKIP:", r.stderr.strip()[-600:])
                return SKIP_EXIT
            first = False
            p = parse(r.stdout)
            tail = r.stdout.strip().splitlines()[-6:]
            print(f"scale {s} render_thread={threaded}: exit={r.returncode}", tail,
                  r.stderr.strip()[-800:])
            if r.returncode or p["failures"] != 0 or any(p[k] is None for k in KEYS):
                ok = False
            runs[threaded] = p
        for k in KEYS:
            if runs[0][k] != runs[1][k]:
                print(f"FAIL scale {s}: {k} differs with the render thread on "
                      f"({runs[0][k]} off, {runs[1][k]} on)")
                ok = False
    # Dynamic resolution: the same level steps between frames, recorded with
    # the thread on (scales above 1 only; 1x has no levels below it).
    for s in [int(v) for v in args.scales.split(",") if v and int(v) > 1]:
        runs = {}
        for threaded in (0, 1):
            r = run([probe, s, threaded, args.frames, "dynres"])
            p = parse(r.stdout)
            tail = r.stdout.strip().splitlines()[-7:]
            print(f"scale {s} dynres render_thread={threaded}: exit={r.returncode}", tail,
                  r.stderr.strip()[-800:])
            if r.returncode or p["failures"] != 0 or any(p[k] is None for k in KEYS):
                ok = False
            runs[threaded] = p
        for k in KEYS:
            if runs[0][k] != runs[1][k]:
                print(f"FAIL scale {s} dynres: {k} differs with the render thread on "
                      f"({runs[0][k]} off, {runs[1][k]} on)")
                ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
