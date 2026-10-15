"""Run actual renderer XR-copy functions against a source-owned ramp on real GL.

Extract the private submission functions verbatim into an isolated fixture:
Windows linkers otherwise retain unrelated public renderer entry points.
No mocks of OpenGL or shader/color conversion are used.
"""
import argparse
import json
import pathlib
import platform
import subprocess


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    while source.index(";", start) < brace:
        start = source.index(signature, start + 1)
        brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if not depth:
                return source[start:pos + 1]
    raise ValueError(signature)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sdl-include", required=True)
    parser.add_argument("--sdl-library", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    dest = pathlib.Path(args.output).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    source = (root / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")
    loader = function(source, "static int load_modern_gl(void)")
    prefix = source[:source.index("static int load_modern_gl(void)")] + loader
    vs_start = source.index("static const char *PRESENT_VS")
    # Extract the shader declaration only. Post-processing helpers now follow
    # it and require the complete presenter, which this focused fixture omits.
    vs = source[vs_start:source.index(";\n", vs_start) + 2]
    pair_start = source.index("typedef struct StereoPair")
    pair = source[pair_start:source.index("} StereoPair;", pair_start) + len("} StereoPair;")]
    declarations = """
static SDL_GLContext s_ctx;
static GLuint s_present_vao, s_xr_color_prog, s_xr_native_tex;
static float s_present_gamma=1.0f;
static GLenum s_last_fbo_status;
static StereoPair s_stereo_pair[2];
static int s_stereo_current, s_native_surface_rect[4];
static GLuint s_xr_surface_program,s_xr_surface_vao,s_xr_surface_vbo,s_xr_surface_tex[2];
static GLuint pt_target_fbo(void) { return 0; } /* no present thread in fixture */
#include "xr_surface_math.h"
"""
    signatures = ["static GLuint compile_shader(", "static GLuint build_program_ex(",
                  "static GLuint build_program(", "static GLuint make_tex(",
                  "static int make_fbo(", "static uint32_t pass_gl_errors(",
                  "static int openxr_color_draw(", "static int openxr_surface_draw(",
                  "static int openxr_copy_eye(",
                  "static int openxr_copy_native("]
    header = prefix + vs + pair + declarations + "\n".join(function(source, s) for s in signatures)
    (dest / "openxr_color_fixture_renderer.h").write_text(header, encoding="utf-8")
    exe = dest / ("openxr_color.exe" if platform.system() == "Windows" else "openxr_color")
    cmd = [args.cc, "-std=gnu11", "-O1", "-DPSX_SDL3=1", "-DPSX_NO_DEBUG_TOOLS=1",
           "-DSDL_MAIN_HANDLED", "-I" + str(root / "runtime/include"),
           "-I" + str(root / "runtime/src"),
           "-I" + args.sdl_include, "-I" + str(dest),
           str(root / "runtime/tests/test_openxr_color_gl.c"), args.sdl_library,
           "-o", str(exe)]
    if platform.system() == "Windows":
        cmd += ["-lopengl32", "-lmingw32", "-luser32", "-lgdi32", "-lwinmm", "-limm32",
                "-lole32", "-loleaut32", "-lversion", "-luuid", "-lsetupapi", "-lcfgmgr32", "-lm"]
    else:
        cmd += ["-lGL", "-lm", "-ldl", "-lpthread"]
    build = subprocess.run(cmd, capture_output=True, text=True)
    receipt = {"command": cmd, "build_exit": build.returncode, "build_stderr": build.stderr}
    if not build.returncode:
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt.update(run_exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
    (dest / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    print("build:", build.returncode, "GL fixture:", receipt.get("run_exit", "not run"))
    if build.returncode:
        print(build.stderr[-4000:])
    elif receipt["run_exit"]:
        print(receipt["stderr"][-2000:])
    return build.returncode or receipt.get("run_exit", 1)


if __name__ == "__main__":
    raise SystemExit(main())
