#!/usr/bin/env python3
"""Keep generated codegen-hash headers out of framework source trees."""

import importlib.util
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def load_compile_overlays():
    path = ROOT / "tools" / "compile_overlays.py"
    spec = importlib.util.spec_from_file_location("compile_overlays", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_hash(path: Path, value: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"#define PSX_OVERLAY_CODEGEN_HASH 0x{value:08X}u\n",
        encoding="utf-8",
    )


def main() -> None:
    recompiler = read("recompiler/CMakeLists.txt")
    runtime = read("runtime/runtime.cmake")
    api = read("runtime/include/overlay_api.h")
    stage = read("tools/stage_setup_sdk.sh")

    source_header = "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include/overlay_codegen_hash.h"
    assert source_header not in recompiler
    assert "${CMAKE_CURRENT_BINARY_DIR}/runtime/include/overlay_codegen_hash.h" in recompiler
    assert "PSXRECOMP_CODEGEN_HASH_INCLUDE_DIR" in runtime
    assert "${CMAKE_CURRENT_BINARY_DIR}/psxrecomp_codegen_include" in runtime
    assert "${PSXRECOMP_CODEGEN_HASH_INCLUDE_DIR}/overlay_codegen_hash.h" in runtime
    assert runtime.index("${PSXRECOMP_CODEGEN_HASH_INCLUDE_DIR}") < runtime.index("${PSXRECOMP_ROOT}/runtime/include")
    assert "__has_include(<overlay_codegen_hash.h>)" in api
    assert "#    include <overlay_codegen_hash.h>" in api
    assert '${GAME_BIN%/*}' in stage
    assert "runtime/include/overlay_codegen_hash.h" in stage
    assert "emitter build has no" in stage

    module = load_compile_overlays()
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        runtime_include = root / "source" / "runtime" / "include"
        source_hash = 0x11111111
        build_hash = 0x22222222
        write_hash(runtime_include / "overlay_codegen_hash.h", source_hash)

        recompiler_exe = root / "build" / "recompiler" / "psxrecomp-game.exe"
        recompiler_exe.parent.mkdir(parents=True)
        recompiler_exe.touch()
        build_header = (recompiler_exe.parent / "runtime" / "include" /
                        "overlay_codegen_hash.h")
        write_hash(build_header, build_hash)
        assert module.codegen_hash(
            str(runtime_include), str(recompiler_exe)) == build_hash

        # The shared tag producer must use the same emitter-owned hash.
        module.codegen_ver = lambda _include: 22
        module.overlay_config_hash = lambda _exe, _toml: 0
        assert module.cache_tag(str(runtime_include), str(recompiler_exe),
                                "unused.toml") == "cg22_22222222_gc00000000_f0"

        release_exe = recompiler_exe.parent / "Release" / recompiler_exe.name
        release_exe.parent.mkdir()
        release_exe.touch()
        assert module.codegen_hash(
            str(runtime_include), str(release_exe)) == build_hash

        build_header.unlink()
        assert module.codegen_hash(
            str(runtime_include), str(recompiler_exe)) == source_hash
        (runtime_include / "overlay_codegen_hash.h").unlink()
        assert module.codegen_hash(str(runtime_include), str(recompiler_exe)) == 0

    print("codegen hash build-tree test: PASS")


if __name__ == "__main__":
    main()
