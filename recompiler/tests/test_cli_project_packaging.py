#!/usr/bin/env python3
"""Regression guards for the self-contained CLI project pipeline."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def check_packaged_codegen_hash() -> None:
    # Exercise the real staging path without rebuilding the tools. The runtime
    # recomputes this hash from the shipped sources; missing inputs must not
    # silently give a different cache identity from psxrecomp-game.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        tools = root / "tools"
        tools.mkdir()
        for name in ("psxrecomp", "psxrecomp-game", "psxrecomp-bios",
                     "psxrecomp-analyze", "psxrecomp-toml"):
            (tools / (name + (".exe" if os.name == "nt" else ""))).touch()
        subprocess.run([
            sys.executable, str(ROOT / "tools/build_cli.py"), "--skip-build",
            "--build-dir", str(tools), "--dist-dir", str(root / "dist"),
        ], check=True)
        frameworks = list((root / "dist").glob("*/framework"))
        assert len(frameworks) == 1, frameworks
        hashes = []
        for index, source in enumerate((ROOT, frameworks[0])):
            header = root / f"hash-{index}.h"
            subprocess.run([
                "cmake", f"-DOUT={header}",
                f"-DPSXRECOMP_CODEGEN_HASH_ROOT={source}",
                "-P", str(source / "runtime/hash_codegen.cmake"),
            ], check=True)
            hashes.append(header.read_bytes())
        assert hashes[0] == hashes[1], (
            "packaged framework codegen hash differs from the tool build; "
            "a canonical input is missing or changed")


def main() -> int:
    cli = (ROOT / "recompiler/src/main_cli.cpp").read_text(encoding="utf-8")
    bios = (ROOT / "recompiler/src/main_bios.cpp").read_text(encoding="utf-8")
    package = (ROOT / "tools/build_cli.py").read_text(encoding="utf-8")
    new_project_cmake = (
        ROOT / "tools/new_project_layout/templates/CMakeLists.txt.in"
    ).read_text(encoding="utf-8")

    require(cli, 'bios_config = \\"psxrecomp/bios/{}\\"',
            "generated game.toml does not name its BIOS profile")
    require(cli, '"--config", fs::absolute(profile_destination).string()',
            "CLI BIOS recompilation does not use the selected profile")
    require(cli, '"--rom", options.bios.string()',
            "CLI BIOS recompilation does not pass the user-selected ROM")
    require(cli, '"--out-dir",',
            "CLI BIOS recompilation does not override the generated output")
    require(cli, 'framework_destination / "generated"',
            "BIOS sources are not emitted where runtime.cmake discovers them")
    require(cli, 'set(PSXRECOMP_BIOS_STEMS \\"{}\\"',
            "generated CMake does not select the emitted BIOS backend")
    require(cli, 'set(PSX_RECOMP_UI OFF CACHE BOOL \\"\\" FORCE)',
            "self-contained CLI projects must not require a game-owned "
            "recomp-ui checkout")
    require(cli, 'PSXRecomp runtime is missing:',
            "generated CLI project does not fail clearly when psxrecomp/ "
            "is missing")
    require(cli, 'git submodule update --init --recursive',
            "generated CLI project missing-runtime error lacks the git "
            "submodule recovery command")
    require(cli, '$RuntimeCMake = Join-Path $Root',
            "generated PowerShell build script lacks a runtime.cmake "
            "preflight")
    require(cli, '[ ! -f \\"$ROOT/psxrecomp/runtime/runtime.cmake\\" ]',
            "generated shell build script lacks a runtime.cmake preflight")
    require(new_project_cmake, 'PSXRecomp runtime is missing:',
            "new-project-layout CMake template does not fail clearly when "
            "psxrecomp/ is missing")
    require(new_project_cmake, 'git submodule update --init --recursive',
            "new-project-layout missing-runtime error lacks the git "
            "submodule recovery command")

    copy_pos = cli.find('fmt::print("[1/4] Copying build framework')
    game_pos = cli.find('fmt::print("[2/4] Recompiling game executable')
    if copy_pos < 0 or game_pos < 0 or copy_pos >= game_pos:
        raise AssertionError(
            "framework/profile must be copied before game recompilation")

    for profile in (
            "OpenBIOS.toml", "SCPH1001.toml",
            "SCPH101.toml", "SCPH5500.toml", "SCPH5501.toml",
            "SCPH5552.toml"):
        require(package, f'"{profile}"',
                f"CLI package omits BIOS profile {profile}")
    require(package, 'shutil.copy2(ROOT / ".gitignore", framework)',
            "packaged framework lacks the project-root marker used by profiles")
    require(package, 'ROOT / "bios" / "openbios.bin"',
            "packaged framework omits the redistributable OpenBIOS image")
    require(package, 'ROOT / "bios" / "OpenBIOS.LICENSE"',
            "packaged framework omits the OpenBIOS license")

    require(bios, 'a == "--rom"',
            "psxrecomp-bios config mode lacks the ROM override")
    require(bios, 'a == "--out-dir"',
            "psxrecomp-bios config mode lacks the output override")

    check_packaged_codegen_hash()
    print("PASS: CLI profiles, stage order, BIOS overrides, and UI isolation agree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
