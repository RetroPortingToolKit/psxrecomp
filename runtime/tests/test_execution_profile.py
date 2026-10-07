"""Link both implementation profiles and check identity/selection failures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def run(args, *, ok=True):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != ok:
        raise AssertionError(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--ninja", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="psx-execution-") as temp:
        source = Path(temp) / "src"
        source.mkdir()
        (source / "main.c").write_text('''#include "execution_identity.h"
#include "netplay_content_gate.h"
#include <stdio.h>
extern int operation(int);
extern int common_game_body(void);
int main(int argc, char** argv) {
    if (common_game_body() != 5) return 4;
    char no_mods[65], modded[65], upper[65];
    if (!psx_execution_content_identity("", no_mods) ||
        !psx_execution_content_identity("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", modded) ||
        !psx_execution_content_identity("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", upper) ||
        strcmp(modded, upper) ||
        psx_execution_content_identity("invalid", modded)) return 1;
    if (argc == 3) {
        NpContentGate local, peer;
        if (!np_content_init(&local, no_mods, 3, 0) ||
            !np_content_init(&peer, argv[1], 3, 1) ||
            !local.enabled || np_content_ready(&local)) return 2;
        for (unsigned i=0; i<4; ++i)
            np_content_note(&local, 1, i, peer.words[2*i], peer.words[2*i+1], 3);
        if (np_content_ready(&local) != (argv[2][0]=='1')) return 3;
    }
    printf("%d %s %s %s\\n", operation(7), PSX_EXECUTION_NAME, PSX_EXECUTION_ID, no_mods);
    return 0;
}
''')
        (source / "hle.c").write_text("int operation(int x) { return x * 3; }\n")
        (source / "common.c").write_text("#ifndef COMMON_SOURCE_FLAG\n#error common source property lost\n#endif\nint common_game_body(void) { return 5; }\n")
        (source / "local_step.h").write_text("#define LOCAL_STEP 3\n")
        (source / "lle.c").write_text('#include "local_step.h"\n#ifndef LLE_SOURCE_FLAG\n#error source compile property lost\n#endif\nint operation(int x) { int s=0; while(x--) s+=LOCAL_STEP; return s; }\n')
        (source / "contract.h").write_text("/* caller contract revision 1 */\n")
        (source / "shared.h").write_text("/* shared service revision 1 */\n")
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(profile_fixture C)
include("{root.as_posix()}/cmake/psx_execution_profile.cmake")
# Match the runtime helper's pre-materialized game shards. They must remain
# automatic configure/hash inputs without creating duplicate Ninja rules.
set_source_files_properties("${{CMAKE_CURRENT_SOURCE_DIR}}/lle.c" PROPERTIES
    GENERATED TRUE COMPILE_DEFINITIONS LLE_SOURCE_FLAG=1)
set_source_files_properties("${{CMAKE_CURRENT_SOURCE_DIR}}/common.c" PROPERTIES
    GENERATED TRUE COMPILE_DEFINITIONS COMMON_SOURCE_FLAG=1)
add_executable(fixture main.c common.c "{root.as_posix()}/runtime/src/psx_sha256.c")
target_include_directories(fixture PRIVATE "{root.as_posix()}/runtime/include")
add_executable(fixture-pgxp main.c common.c "{root.as_posix()}/runtime/src/psx_sha256.c")
target_include_directories(fixture-pgxp PRIVATE "{root.as_posix()}/runtime/include")
psxrecomp_execution_profile(fixture CONTRACT_FILES shared.h)
psxrecomp_execution_profile(fixture-pgxp CONTRACT_FILES shared.h)
psxrecomp_add_implementation(fixture NAME triple CONTRACT integer-v1
    HLE_SOURCES hle.c LLE_SOURCES lle.c CONTRACT_FILES contract.h common.c)
''')

        def configure(profile, name=None, ok=True):
            build = Path(temp) / (name or profile)
            run([args.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                 "-DCMAKE_C_COMPILER=" + args.cc, "-DCMAKE_MAKE_PROGRAM=" + args.ninja,
                 "-DPSX_EXECUTION_PROFILE=" + profile], ok=ok)
            return build

        def build_run(build):
            run([args.cmake, "--build", str(build), "--parallel", "2"])
            exe = build / ("fixture.exe" if (build / "fixture.exe").exists() else "fixture")
            values = run([str(exe)]).strip().split()
            manifest = json.loads((build / "fixture.execution.json").read_text())
            sibling = json.loads((build / "fixture-pgxp.execution.json").read_text())
            assert sibling == manifest, "auto-clone must use the same implementation contract"
            assert values[0] == "21", values
            assert values[1] == manifest["profile"], values
            assert values[2] == manifest["identity"], values
            assert values[3] == hashlib.sha256(("psx-session-execution-v1:" + values[2]).encode()).hexdigest()
            return manifest

        enhanced_build = configure("ENHANCED")
        enhanced = build_run(enhanced_build)
        reference = build_run(configure("REFERENCE"))
        assert enhanced["identity"] != reference["identity"]
        # The reference body is maintained once, and modifications still
        # refresh its selected-source fingerprint without manual configure.
        (source / "lle.c").write_text("int operation(int x) { int s=0; for(int i=0;i<x;++i) s+=3; return s; }\n")
        reference_changed = build_run(Path(temp) / "REFERENCE")
        assert reference_changed["identity"] != reference["identity"]
        reference = reference_changed
        # Exercise the real wire gate, including sessions without mods.
        fixture = enhanced_build / ("fixture.exe" if (enhanced_build / "fixture.exe").exists() else "fixture")
        def session_id(manifest):
            return hashlib.sha256(("psx-session-execution-v1:" + manifest["identity"]).encode()).hexdigest()
        run([str(fixture), session_id(enhanced), "1"])
        run([str(fixture), session_id(reference), "0"])
        configure("AUTO", ok=False)
        # Relocating the entire source tree must not change portable identities.
        import shutil
        relocated = Path(temp) / "relocated"
        shutil.copytree(source, relocated)
        saved = source
        source = relocated
        assert build_run(configure("ENHANCED", "relocated-build"))["identity"] == enhanced["identity"]
        source = saved
        # A changed selected implementation or contract changes the identity.
        (source / "hle.c").write_text("int operation(int x) { return x + x + x; }\n")
        changed = build_run(enhanced_build)  # automatic CMake reconfigure
        assert changed["identity"] != enhanced["identity"]
        assert build_run(configure("REFERENCE"))["identity"] == reference["identity"]
        (source / "contract.h").write_text("/* caller contract revision 2 */\n")
        changed_contract = build_run(enhanced_build)
        assert changed_contract["identity"] != changed["identity"]
        (source / "shared.h").write_text("/* shared service revision 2 */\n")
        assert build_run(enhanced_build)["identity"] != changed_contract["identity"]
        # Common generated bodies are compiled in both profiles and hashed as
        # caller contracts. Their edits must also reconfigure automatically.
        common_before = build_run(enhanced_build)
        reference_before = build_run(Path(temp) / "REFERENCE")
        (source / "common.c").write_text("int common_game_body(void) { return 2 + 3; }\n")
        assert build_run(enhanced_build)["identity"] != common_before["identity"]
        assert build_run(Path(temp) / "REFERENCE")["identity"] != reference_before["identity"]
        # Private HLE input is not required to build the maintained reference.
        (source / "hle.c").unlink()
        build_run(configure("REFERENCE", "reference-no-private"))
        configure("ENHANCED", "missing-selected", ok=False)
    print("execution profile link, ABI, identity and failure checks passed")


if __name__ == "__main__":
    main()
