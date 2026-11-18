#!/usr/bin/env python3
"""The setup host's hand-over to the product honours a headless start.

The setup exe of a built game is the setup host. Once the game is built it
starts the product in build-release and exits. It added --launcher to that
start, always, so that the product's persisted skip_launcher setting cannot
hide the launcher on a hand-over. In the product --launcher wins over
--no-launcher, --headless, PSX_NO_LAUNCHER and PSX_HEADLESS. So a scripted
headless start of the setup exe always waited in the launcher and showed no
frame from the game it handed over to.

This builds the real host translation unit into a stand-in setup program, puts
a stand-in product where the host looks for it, and reads the arguments the
product was started with:

  a player's start      --launcher is added, once, after the caller's arguments;
  a headless start      one of the signals the product honours itself, read by
                        the product's own rules: --no-launcher, --headless,
                        --headless-opengl, --hidden-window; PSX_NO_LAUNCHER
                        present, whatever its value; PSX_HEADLESS set to
                        something other than empty or a value that starts with
                        0. The arguments are forwarded as they are, with no
                        --launcher;
  not a headless start  PSX_HEADLESS=0.

Without recomp-ui or a C compiler the test cannot run: it says SKIP and exits
77, which ctest reports as "Skipped", not as "Passed" (SKIP_RETURN_CODE).

The setup host is a real program with a toolchain installer in it, and a
wrong start could make it remove and reinstall the build toolchain. So the
host here runs in a closed environment (host_environment):

  - no case may pass a switch or variable that asks for a build
    (FORBIDDEN_ARGS, FORBIDDEN_VARS);
  - the environment is built from nothing, not copied: every folder the host
    derives a toolchain or data root from (LOCALAPPDATA, APPDATA, USERPROFILE,
    HOME, XDG_DATA_HOME, RETCOMM_DATA_HOME, RETCOMM_TOOLCHAIN_CACHE, TEMP, TMP)
    is a folder inside the test's own temporary folder, PATH holds the system
    folders only, and no variable names a toolchain;
  - the proxy variables point at a closed local port, so a download cannot work;
  - after the cases the toolchain and data folders must still be empty. A file
    there means the host tried to install something: the test fails.

--dry-run prints that environment and the cache bases the host derives from it,
checks that every root is inside the temporary folder, and starts nothing.
"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"
SKIPPED = 77     # runtime/CMakeLists.txt: SKIP_RETURN_CODE of this test

SETUP_HOST = """
#include <string.h>
#include <stdio.h>
#include "psxrecomp_codegen_host.h"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
int main(int argc, char** argv) {
    PsxrecompCodegenHostConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmake_target       = "psx-runtime";
    cfg.exe_basename       = "Probe";
    cfg.gen_marker_relpath = "generated/SCUS_943.51_dispatch.c";
    psxrecomp_codegen_host_forward_if_built(&cfg, argc, argv);
    puts("NOT FORWARDED");   /* the hand-over replaces or ends this process */
    return 3;
}
"""

# The product: it writes the arguments it was started with, one per line.
PRODUCT = """
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    const char* out = getenv("HANDOVER_ARGS_OUT");
    FILE* f = out ? fopen(out, "wb") : NULL;
    int i;
    if (!f) return 4;
    fprintf(f, "started\\n");
    for (i = 1; i < argc; ++i) fprintf(f, "%s\\n", argv[i]);
    fclose(f);
    return 0;
}
"""

# Switches and variables that could make the host build or install. No case may use them.
FORBIDDEN_ARGS = ("--diagnostic", "--diagnostic-only", "--collect-diagnostics")
FORBIDDEN_VARS = ("PSXRECOMP_DIAGNOSTIC", "PSXRECOMP_FORCE_SETUP", "RETCOMM_TOOLCHAIN_DIR",
                  "PSXRECOMP_TOOLCHAIN_DIR", "TOOLCHAIN_DIR", "BPE_TOOLCHAIN_DIR", "CMAKE", "PYTHON",
                  "RETCOMM_PYTHON")

# Folders the host derives a toolchain or data root from. Each becomes a folder
# of the test; they must be empty when the cases are over.
SANDBOX_ROOTS = {
    "LOCALAPPDATA": "host-localappdata",
    "APPDATA": "host-appdata",
    "USERPROFILE": "host-home",
    "HOME": "host-home",
    "XDG_DATA_HOME": "host-xdg-data",
    "RETCOMM_DATA_HOME": "host-retcomm-data",
    "RETCOMM_TOOLCHAIN_CACHE": "host-retcomm-toolchain-cache",
}
DEAD_PROXY = "http://127.0.0.1:9"


def host_environment(sandbox: Path) -> dict[str, str]:
    """The only environment the setup host is started with. Nothing is copied
    from the caller except the names the system needs to start a program."""
    env: dict[str, str] = {}
    if os.name == "nt":
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        env["SystemRoot"] = system_root
        env["windir"] = system_root
        env["SystemDrive"] = os.environ.get("SystemDrive", "C:")
        env["ComSpec"] = str(Path(system_root) / "System32" / "cmd.exe")
        env["PATHEXT"] = ".COM;.EXE;.BAT;.CMD"
        env["PATH"] = os.pathsep.join([str(Path(system_root) / "System32"), system_root])
    else:
        env["PATH"] = "/usr/bin:/bin"
    for name, folder in SANDBOX_ROOTS.items():
        (sandbox / folder).mkdir(parents=True, exist_ok=True)
        env[name] = str(sandbox / folder)
    (sandbox / "host-temp").mkdir(parents=True, exist_ok=True)
    for name in ("TEMP", "TMP", "TMPDIR"):
        env[name] = str(sandbox / "host-temp")
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env[name] = DEAD_PROXY
        if os.name != "nt":   # Windows names are case-blind; one spelling is enough
            env[name.lower()] = DEAD_PROXY
    return env


def roots_outside(env: dict[str, str], sandbox: Path) -> list[str]:
    """Names of the host's toolchain, data and temp roots that are not inside the
    test's own folder. Must be empty before any program is started."""
    inside = sandbox.resolve()
    return [name for name in list(SANDBOX_ROOTS) + ["TEMP", "TMP", "TMPDIR"]
            if name not in env or inside not in Path(env[name]).resolve().parents]


def dry_run() -> int:
    """Builds nothing and starts nothing: prints the environment a host would get."""
    with tempfile.TemporaryDirectory() as tmp:
        sandbox = Path(tmp) / "sandbox"
        env = host_environment(sandbox)
        print("dry run: nothing is built and no program is started")
        print("the test's temporary folder: %s" % tmp)
        for name in sorted(env):
            print("  %s=%s" % (name, env[name]))
        print("the host's toolchain cache bases, from its own rules (collect_toolchain_cache_bases):")
        for base in (env["RETCOMM_TOOLCHAIN_CACHE"],
                     str(Path(env["RETCOMM_DATA_HOME"]) / "toolchains" / "cmake-clang-v1"),
                     str(Path(env["LOCALAPPDATA"]) / "retcomm" / "toolchains" / "cmake-clang-v1"),
                     str(Path(env["LOCALAPPDATA"]) / "psxrecomp" / "toolchains" / "cmake-clang-v1"),
                     str(Path(env["XDG_DATA_HOME"]) / "retcomm" / "toolchains" / "cmake-clang-v1"),
                     str(Path(env["HOME"]) / ".local" / "share" / "retcomm" / "toolchains" / "cmake-clang-v1")):
            print("  %s" % base)
        outside = roots_outside(env, sandbox)
        print("roots outside the temporary folder: %s" % (", ".join(outside) if outside else "none"))
        print("forbidden switches: %s" % ", ".join(FORBIDDEN_ARGS))
        print("forbidden variables: %s" % ", ".join(FORBIDDEN_VARS))
        return 1 if outside else 0


def sandbox_leftovers(sandbox: Path) -> list[str]:
    """Files the host left in its toolchain and data folders. There must be none."""
    found = []
    for folder in sorted(set(SANDBOX_ROOTS.values())):
        for path in sorted((sandbox / folder).rglob("*")):
            found.append(str(path.relative_to(sandbox)))
    return found


def find_recomp_ui() -> Path | None:
    env = os.environ.get("RECOMP_UI_ROOT")
    candidates = ([Path(env)] if env else []) + [
        ROOT.parent / "recomp-ui",
        ROOT / "recomp-ui",
    ]
    for c in candidates:
        if (c / "src" / "recomp_launcher.h").is_file():
            return c
    return None


def make_project(root: Path, framework: str) -> None:
    """A built project as the host sees it: sources generated, a product folder."""
    (root / "generated").mkdir(parents=True)
    (root / "generated" / "SCUS_943.51_dispatch.c").write_text("", encoding="utf-8")
    fw_gen = root / framework / "generated"
    fw_gen.mkdir(parents=True)
    (fw_gen / "SCPH5552_dispatch.c").write_text(
        "const PsxBiosBackend SCPH5552_psx_bios_backend = {};\n", encoding="utf-8")
    (fw_gen / "SCPH5552_full.c").write_text("", encoding="utf-8")
    (root / "game.toml").write_text("[game]\n", encoding="utf-8")
    (root / "psxrecomp").mkdir(exist_ok=True)
    (root / "psxrecomp" / "psxrecomp_cli.py").write_text("", encoding="utf-8")
    (root / "build-release").mkdir()


def main() -> int:
    if "--dry-run" in sys.argv[1:]:
        return dry_run()
    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return SKIPPED
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return SKIPPED
    suffix = ".exe" if os.name == "nt" else ""
    framework = "psxrecomp"

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "host.c").write_text(SETUP_HOST, encoding="utf-8")
        (tmp / "product.c").write_text(PRODUCT, encoding="utf-8")
        host_exe = tmp / ("setup-host" + suffix)
        product_exe = tmp / ("product" + suffix)
        for what, argv in (
            ("the setup host", [cc, "-std=c11", "-o", str(host_exe), str(tmp / "host.c"), str(HOST_C),
                                '-DPSX_SETUP_BIOS_STEMS="SCPH5552"', f'-DPSX_SETUP_FRAMEWORK_REL="{framework}"',
                                "-I", str(ROOT / "host"), "-I", str(ROOT / "runtime" / "include"),
                                "-I", str(ui / "src"), "-I", str(ui / "src" / "common")]),
            ("the product", [cc, "-std=c11", "-o", str(product_exe), str(tmp / "product.c")]),
        ):
            build = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
            if build.returncode != 0:
                print("FAIL: could not build %s\n%s" % (what, build.stderr[-2000:]))
                return 1

        launcher = ["--launcher"]
        # (arguments, variables, what the product must get, why)
        cases = [
            ([], {}, launcher,
             "a player's start: the launcher is forced"),
            (["--bios", "b i o s.bin"], {}, ["--bios", "b i o s.bin"] + launcher,
             "a player's start with arguments: they are forwarded and --launcher follows"),
            (["--launcher"], {}, launcher,
             "--launcher given by the caller is not doubled"),
            ([], {"PSX_HEADLESS": "0"}, launcher,
             "PSX_HEADLESS=0 is not a headless start (the product's rule)"),
            ([], {"PSX_NO_LAUNCHER": "0"}, [],
             "PSX_NO_LAUNCHER present, whatever its value: no --launcher (the product's rule)"),
            ([], {"PSX_HEADLESS": "1"}, [],
             "PSX_HEADLESS=1: no --launcher"),
            ([], {"PSX_NO_LAUNCHER": "1"}, [],
             "PSX_NO_LAUNCHER=1: no --launcher"),
            (["--no-launcher"], {}, ["--no-launcher"],
             "--no-launcher is forwarded as it is"),
            (["--headless", "--disc", "d.cue"], {}, ["--headless", "--disc", "d.cue"],
             "--headless is forwarded as it is"),
            (["--headless-opengl"], {}, ["--headless-opengl"],
             "--headless-opengl is forwarded as it is"),
            (["--hidden-window"], {}, ["--hidden-window"],
             "--hidden-window is forwarded as it is"),
            (["--bios", "b.bin"], {"PSX_HEADLESS": "1", "PSX_NO_LAUNCHER": "1"}, ["--bios", "b.bin"],
             "a scripted start with both variables: arguments only"),
        ]
        for args, variables, _expected, why in cases:
            asked = [a for a in args if a in FORBIDDEN_ARGS] + [v for v in variables if v in FORBIDDEN_VARS]
            if asked:
                print("FAIL: the case %r asks the host for a build or an install (%s); "
                      "this test must never do that" % (why, ", ".join(asked)))
                return 1

        sandbox = tmp / "sandbox"
        failures = 0
        for index, (args, variables, expected, why) in enumerate(cases):
            project = tmp / f"case {index}"
            make_project(project, framework)
            shutil.copy2(product_exe, project / "build-release" / ("Probe" + suffix))
            out = project / "handover-args.txt"
            env = host_environment(sandbox)
            env.update(variables)
            env["PSXRECOMP_PROJECT_ROOT"] = str(project)
            env["HANDOVER_ARGS_OUT"] = str(out)
            outside = roots_outside(env, sandbox)
            if outside:
                print("FAIL: the host would be started with a folder outside the test's own: %s"
                      % ", ".join(outside))
                return 1
            run = subprocess.run([str(host_exe)] + args, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", env=env, cwd=str(tmp))
            # On Windows the host starts the product and exits without waiting for it.
            deadline = time.time() + 15.0
            got = None
            while time.time() < deadline:
                if out.is_file():
                    lines = out.read_text(encoding="utf-8", errors="replace").splitlines()
                    if lines and lines[0] == "started":
                        got = lines[1:]
                        break
                time.sleep(0.05)
            if "NOT FORWARDED" in run.stdout:
                print("FAIL: %s\n  the host did not hand over: %s" % (why, run.stderr.strip()[-400:]))
                failures += 1
            elif got != expected:
                print("FAIL: %s\n  expected %r, the product got %r" % (why, expected, got))
                failures += 1
            else:
                print("ok: %s" % why)
        leftovers = sandbox_leftovers(sandbox)
        if leftovers:
            print("FAIL: the host wrote into its toolchain or data folders; a hand-over must not:\n  %s"
                  % "\n  ".join(leftovers[:20]))
            failures += 1
        else:
            print("ok: the host left its toolchain and data folders empty")
        if failures:
            return 1

    print("PASS: the hand-over forces the launcher for a player and honours a headless start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
