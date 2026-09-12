#!/usr/bin/env python3
"""Regression: the codegen host must accept any configured, linkable BIOS stem.

bios_backends_missing() in host/psxrecomp_codegen_host.c used to probe two
hardcoded filenames, psxrecomp/generated/OpenBIOS_dispatch.c and
SCPH1001_dispatch.c. Ports that pin another image via recompiler.bios_config /
PSXRECOMP_BIOS_STEMS emit <stem>_dispatch.c + <stem>_full.c instead, so the
probe could never be satisfied: Generate succeeded, the setup wizard reopened,
and first-run setup looped forever. Every wave-3 kit pins SCPH5552 and every
one of them shipped with that loop.

test_cli_retail_bios_profile.py already covers the psxrecomp_cli.py half of
this feature. This is the C host half, which had no coverage.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"

PROBE = """
#include <string.h>
#include <stdio.h>
#include "psxrecomp_codegen_host.h"
/* recomp-ui symbol the host references but sources_missing() never reaches. */
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
int main(void) {
    PsxrecompCodegenHostConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmake_target       = "psx-runtime";
    cfg.exe_basename       = "Probe";
    cfg.gen_marker_relpath = "generated/SCUS_943.51_dispatch.c";
    printf("%d", psxrecomp_codegen_host_sources_missing(&cfg));
    return 0;
}
"""


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


def make_project(root: Path, bios_files: list[str], framework: str, descriptor: bool) -> None:
    """A project tree the host recognises, with the given BIOS artefacts."""
    (root / "generated").mkdir(parents=True, exist_ok=True)
    (root / "generated" / "SCUS_943.51_dispatch.c").write_text("", encoding="utf-8")
    fw_gen = root / framework / "generated"
    fw_gen.mkdir(parents=True, exist_ok=True)
    (root / "game.toml").write_text("[game]\n", encoding="utf-8")
    (root / "psxrecomp").mkdir(exist_ok=True)
    (root / "psxrecomp" / "psxrecomp_cli.py").write_text("", encoding="utf-8")
    for name in bios_files:
        text = ""
        if descriptor and name.endswith("_dispatch.c"):
            text = "const PsxBiosBackend " + name.removesuffix("_dispatch.c") + "_psx_bios_backend = {};\n"
        (fw_gen / name).write_text(text, encoding="utf-8")


def main() -> int:
    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return 0
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        probe_c = tmp / "probe.c"
        probe_c.write_text(PROBE, encoding="utf-8")
        exe = tmp / ("probe.exe" if os.name == "nt" else "probe")
        framework = "framework checkout"
        build = subprocess.run(
            [cc, "-std=c11", "-o", str(exe), str(probe_c), str(HOST_C),
             '-DPSX_SETUP_BIOS_STEMS="OpenBIOS|SCPH5552"',
             f'-DPSX_SETUP_FRAMEWORK_REL="{framework}"',
             "-I", str(ROOT / "host"),
             "-I", str(ROOT / "runtime" / "include"),
             "-I", str(ui / "src"), "-I", str(ui / "src" / "common")],
            capture_output=True, text=True)
        if build.returncode != 0:
            print("FAIL: could not build probe\n" + build.stderr[-2000:])
            return 1

        cases = [
            # (BIOS artefacts present, expected sources_missing, why)
            (["SCPH5552_dispatch.c", "SCPH5552_full.c"], True, 0,
             "a pinned non-SCPH1001 stem must satisfy the host"),
            (["OpenBIOS_dispatch.c", "OpenBIOS_full.c"], True, 0,
             "the bundled stem must still satisfy the host"),
            (["SCPH5552_dispatch.c"], True, 1,
             "a dispatch with no _full.c is not a linkable backend"),
            (["SCPH5552_dispatch.c", "SCPH5552_full.c"], False, 1,
             "a pre-descriptor pair must still require generation"),
            (["SCPH1001_dispatch.c", "SCPH1001_full.c"], True, 1,
             "an unrequested backend must not bypass setup"),
            (["SCUS_943.51_dispatch.c", "SCUS_943.51_full.c"], True, 1,
             "game sources cannot masquerade as a configured BIOS"),
            ([], False, 1,
             "no BIOS backend at all means setup is genuinely incomplete"),
        ]
        failures = 0
        for index, (artefacts, descriptor, expected, why) in enumerate(cases):
            project = tmp / f"case{index}"
            make_project(project, artefacts, framework, descriptor)
            env = dict(os.environ, PSXRECOMP_PROJECT_ROOT=str(project))
            run = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            got = run.stdout.strip()
            if got != str(expected):
                print("FAIL: %s\n  artefacts=%s expected=%s got=%r"
                      % (why, artefacts or "(none)", expected, got))
                failures += 1
            else:
                print("ok: %s" % why)
        if failures:
            return 1

    print("PASS: codegen host accepts configured, linkable BIOS stems in relocated frameworks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
