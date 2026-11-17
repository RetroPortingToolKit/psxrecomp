#!/usr/bin/env python3
"""Regression: the setup host hands the recorded disc set and the settings to
the product.

The wizard writes disc.cfg at the project root and tries build-release/ as
well, but before the first build build-release/ does not exist when the disc
is picked, so that copy fails silently. The runtime reads disc.cfg only beside
its own executable. The first start of the built game therefore asked for the
disc again, while the BIOS was remembered through its project-root fallback.
settings.toml beside the setup executable was not handed over either.

sync_project_disc_cfg_to_product() and sync_project_settings_to_product()
close that gap on both hand-off routes (relaunch after Generate, and forward
on later starts). This compiles the host translation unit into a probe, calls
the relaunch route and checks the copy rules.
"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HOST_C = ROOT / "host" / "psxrecomp_codegen_host.c"

PROBE = """
#include "psxrecomp_codegen_host.c"
int recomp_launcher_relaunch_exe(char* o, size_t c) { (void)o; (void)c; return 0; }
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    snprintf(g_project_root, sizeof(g_project_root), "%s", argv[1]);
    persist_relaunch_sidecars(argv[2], NULL);
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


def main() -> int:
    failures = 0

    def check(ok: bool, why: str, detail: str = "") -> None:
        nonlocal failures
        if ok:
            print("ok: " + why)
        else:
            failures += 1
            print("FAIL: " + why + (("\n  " + detail) if detail else ""))

    # The forward route (later starts) must hand over both files as well; it
    # had no sidecar handling at all. Definition plus two calls each.
    host_text = HOST_C.read_text(encoding="utf-8")
    for name in ("sync_project_disc_cfg_to_product(", "sync_project_settings_to_product("):
        check(host_text.count(name) >= 3,
              "%s is defined and called from relaunch and forward" % name.rstrip("("),
              "found %d" % host_text.count(name))

    ui = find_recomp_ui()
    if ui is None:
        print("SKIP: recomp-ui not found (set RECOMP_UI_ROOT)")
        return 1 if failures else 0
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        print("SKIP: no C compiler on PATH")
        return 1 if failures else 0

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        probe_c = tmp / "probe.c"
        probe_c.write_text(PROBE, encoding="utf-8")
        exe = tmp / ("probe.exe" if os.name == "nt" else "probe")
        build = subprocess.run(
            [cc, "-std=gnu11", "-o", str(exe), str(probe_c),
             '-DPSX_SETUP_BIOS_STEMS="SCPH1001"',
             '-DPSX_SETUP_FRAMEWORK_REL="psxrecomp"',
             "-I", str(ROOT / "host"),
             "-I", str(ROOT / "runtime" / "include"),
             "-I", str(ui / "src"), "-I", str(ui / "src" / "common")],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        if build.returncode != 0:
            print("FAIL: could not build probe\n" + build.stderr[-2000:])
            return 1

        def relaunch(proj: Path, build_dir: Path) -> subprocess.CompletedProcess:
            return subprocess.run([str(exe), str(proj), str(build_dir / "Game.exe")],
                                  cwd=str(proj), capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=60)

        two_discs = "C:/Games/Example (Disc 1).chd\nC:/Games/Example (Disc 2).chd"
        # (project disc.cfg or None, product disc.cfg or None, expected product disc.cfg or None, why)
        cases = [
            (two_discs, None, two_discs + "\n",
             "a missing product disc.cfg receives the whole recorded set"),
            ("C:/Games/one.cue", "", "C:/Games/one.cue\n",
             "an empty product disc.cfg is replaced"),
            (two_discs, "D:/picked in product.cue\n", "D:/picked in product.cue\n",
             "a pick made in the product is never overwritten"),
            (None, None, None,
             "no recorded disc means no product disc.cfg is invented"),
        ]
        for index, (project, product, expected, why) in enumerate(cases):
            proj = tmp / f"case {index}"
            build_dir = proj / "build-release"
            build_dir.mkdir(parents=True)
            if project is not None:
                (proj / "disc.cfg").write_bytes((project + "\n").encode("utf-8"))
            if product is not None:
                (build_dir / "disc.cfg").write_bytes(product.encode("utf-8"))
            run = relaunch(proj, build_dir)
            target = build_dir / "disc.cfg"
            got = target.read_text(encoding="utf-8") if target.is_file() else None
            if got is not None:
                got = got.replace("\r\n", "\n")
            check(run.returncode == 0 and got == expected, why,
                  "expected=%r got=%r rc=%d" % (expected, got, run.returncode))

        # The same relaunch route seeds the product's settings.toml.
        settings = (b'\xef\xbb\xbf[video]\r\nrenderer="opengl"\r\nsupersampling=4\r\n'
                    + b"# retained comment\n" * 1100)
        settings_cases = [
            (settings, None, settings,
             "relaunch carries every settings byte, BOM and comments included"),
            (settings, b"[video]\nsupersampling=2\n", b"[video]\nsupersampling=2\n",
             "relaunch keeps existing product settings"),
            (settings, b"", b"", "relaunch keeps even an empty product settings file"),
            (None, None, None, "relaunch invents no settings when setup has none"),
        ]
        for index, (source, product, expected, why) in enumerate(settings_cases):
            proj = tmp / f"settings {index}"
            build_dir = proj / "build-release"
            build_dir.mkdir(parents=True)
            if source is not None:
                (proj / "settings.toml").write_bytes(source)
            if product is not None:
                (build_dir / "settings.toml").write_bytes(product)
            run = relaunch(proj, build_dir)
            target = build_dir / "settings.toml"
            got = target.read_bytes() if target.is_file() else None
            check(run.returncode == 0 and got == expected, why,
                  "expected %s bytes, got %s, rc=%d" % (
                      None if expected is None else len(expected),
                      None if got is None else len(got), run.returncode))

        # A temporary file that is already there is not ours to remove or use.
        proj = tmp / "settings blocked"
        build_dir = proj / "build-release"
        build_dir.mkdir(parents=True)
        (proj / "settings.toml").write_bytes(settings)
        sentinel = build_dir / "settings.toml.setup.tmp"
        sentinel.write_bytes(b"foreign sentinel")
        run = relaunch(proj, build_dir)
        check(run.returncode == 0 and not (build_dir / "settings.toml").exists()
              and sentinel.read_bytes() == b"foreign sentinel",
              "a temporary file that is already there is left alone and not used")

    if failures:
        print("%d check(s) failed" % failures)
        return 1
    print("PASS: setup host hands the recorded disc set and settings to the product")
    return 0


if __name__ == "__main__":
    sys.exit(main())
