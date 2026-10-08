"""Build and run test_gl_frame_gen.c on a hidden real OpenGL context.

Frame generation ([video] frame_generation, docs/FRAME_GENERATION.md): per
scale and present path (VRAM, native-wide), the fixture runs with the render
thread on and generation off, then on (forced). The real presented images, in
order, must be identical; the run with generation on must have presented
generated frames and passes its own endpoint checks (phase 1 = the newer real
image, phase 0 = the older one; in redraw, the default, nothing of
reprojection ran or was allocated). Then with reprojection
(FG_METHOD=reprojection, with and without PGXP depth): the warp's pixel checks
pass and the real images are those of the run with generation off. Build
flags and skips as
run_gl_render_thread.py.
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
KEYS = ("real",)


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
    ap.add_argument("--scales", default="1,3")
    ap.add_argument("--frames", default="24")
    args = ap.parse_args()
    sdl_includes = [str(pathlib.Path(d).resolve()) for d in args.sdl_include.split(";") if d]
    args.sdl_library = str(pathlib.Path(args.sdl_library).resolve())
    framework = pathlib.Path(__file__).resolve().parents[2]
    fixture = args.fixture or framework / "runtime/tests/test_gl_frame_gen.c"
    out_root = pathlib.Path(args.output).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    dest = pathlib.Path(tempfile.mkdtemp(prefix="fg-", dir=out_root))
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

    ok = True
    first = True
    for s in [int(v) for v in args.scales.split(",") if v]:
        for path, timing in (("vram", "flip"), ("wide", "flip"), ("vram", "late"), ("wide", "late")):
            runs = {}
            for fg in (0, 1):
                r = run([probe, s, fg, path, args.frames, timing])
                if first and r.returncode == SKIP_EXIT:
                    print("SKIP:", r.stderr.strip()[-600:])
                    return SKIP_EXIT
                first = False
                p = parse(r.stdout)
                tail = r.stdout.strip().splitlines()[-6:]
                print(f"scale {s} {path} {timing} frame_generation={fg}: exit={r.returncode}", tail,
                      r.stderr.strip()[-800:])
                if r.returncode or p["failures"] != 0 or p["real"] is None:
                    ok = False
                m = re.search(r"^presents=(\d+) generated=(\d+)$", r.stdout, re.M)
                p["presents"] = int(m[1]) if m else -1
                p["generated"] = int(m[2]) if m else -1
                runs[fg] = p
            if runs[0]["real"] != runs[1]["real"] or runs[0]["presents"] != runs[1]["presents"]:
                print(f"FAIL scale {s} {path} {timing}: the real frames differ with generation on "
                      f"({runs[0]['real']}/{runs[0]['presents']} off, "
                      f"{runs[1]['real']}/{runs[1]['presents']} on)")
                ok = False
            if runs[0]["generated"] != 0 or runs[1]["generated"] <= 0:
                print(f"FAIL scale {s} {path} {timing}: generated frames off={runs[0]['generated']} "
                      f"on={runs[1]['generated']}")
                ok = False
            # Reprojection ([video] frame_generation_method = "reprojection",
            # opt-in): the fixture checks the warp's pixels (and with PGXP
            # depth, that the warp leaves no depth for the cars); the real
            # frames stay those of the run with generation off.
            # pan 1 (the scene above, with PGXP depth): its real frames must be
            # the generation-off run's; pan 6: a camera move the warp must follow.
            for pgxp, pan in (("1", "1"), ("0", "6")):
                env = dict(os.environ, FG_METHOD="reprojection", FG_PGXP=pgxp, FG_PAN=pan)
                command = [str(c) for c in (probe, s, 1, path, args.frames, timing)]
                r = subprocess.run(command, cwd=dest, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", env=env)
                receipt.append({"cmd": command, "env": {"FG_METHOD": "reprojection", "FG_PGXP": pgxp, "FG_PAN": pan},
                                "exit": r.returncode, "stdout": r.stdout, "stderr": r.stderr[-4000:]})
                (dest / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
                p = parse(r.stdout)
                info = [l for l in r.stdout.splitlines()
                        if l.startswith(("reproject", "pgxp", "presents"))]
                print(f"scale {s} {path} {timing} reprojection pgxp={pgxp} pan={pan}: exit={r.returncode}", info,
                      r.stderr.strip()[-800:])
                m = re.search(r"^presents=(\d+) generated=(\d+)$", r.stdout, re.M)
                if (r.returncode or p["failures"] != 0 or not m or int(m[2]) <= 0
                        or (pan == "1" and p["real"] != runs[0]["real"])):
                    print(f"FAIL scale {s} {path} {timing} reprojection pgxp={pgxp} pan={pan}")
                    ok = False
            # Present thread ([video] present_thread): the fixture checks the
            # window shows every composed image bit for bit, in order. The real
            # images match the direct-swap run: bit for bit, except that
            # rasterizing into a texture instead of the window's drawable may
            # round a filtered present pixel differently (seen on Apple's
            # GL-on-Metal: 1 LSB, a pixel or two per frame); allowed up to 1 LSB
            # on at most 0.05 % of the pixels, and reported.
            for fg in (0, 1):
                dirs = {}
                for mode in ("direct", "pt"):
                    sub = dest / f"pt-{s}-{path}-{timing}-{fg}-{mode}"
                    sub.mkdir(exist_ok=True)
                    cmd = [str(c) for c in (probe, s, fg, path, args.frames, timing)]
                    if mode == "pt":
                        cmd.append("pt")
                    env = dict(os.environ, FG_DUMP="1")
                    r = subprocess.run(cmd, cwd=sub, capture_output=True, text=True,
                                       encoding="utf-8", errors="replace", env=env)
                    p = parse(r.stdout)
                    if r.returncode or p["failures"] != 0:
                        print(f"FAIL scale {s} {path} {timing} fg={fg} {mode}: exit={r.returncode}",
                              r.stdout.strip().splitlines()[-4:], r.stderr.strip()[-800:])
                        ok = False
                    dirs[mode] = sub
                files = sorted(f.name for f in dirs["direct"].glob("real*.rgba"))
                other = sorted(f.name for f in dirs["pt"].glob("real*.rgba"))
                if files != other or not files:
                    print(f"FAIL scale {s} {path} {timing} fg={fg}: present thread real frames "
                          f"{len(other)} vs {len(files)}")
                    ok = False
                    continue
                exact, worst, worst_px = 0, 0, 0.0
                for name in files:
                    a = (dirs["direct"] / name).read_bytes()
                    b = (dirs["pt"] / name).read_bytes()
                    if a == b:
                        exact += 1
                        continue
                    d = [abs(x - y) for x, y in zip(a, b) if x != y]
                    px = len({i // 4 for i, (x, y) in enumerate(zip(a, b)) if x != y})
                    worst = max(worst, max(d) if d else 0)
                    worst_px = max(worst_px, px / (len(a) / 4))
                print(f"scale {s} {path} {timing} frame_generation={fg} present_thread=1: "
                      f"{exact}/{len(files)} real frames bit-identical, max diff {worst} LSB "
                      f"on {worst_px * 100:.3f} % of pixels")
                if worst > 1 or worst_px > 0.0005:
                    print(f"FAIL scale {s} {path} {timing} fg={fg}: present thread changed real frames")
                    ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
