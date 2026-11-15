"""Compile real generated/plugin fixtures; perturb inputs without changing ID."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(args, ok=True):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != ok:
        raise AssertionError(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cxx")
    parser.add_argument("--ninja", required=True)
    args = parser.parse_args()
    sdk = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="psx-build-manifest-") as temp:
        base = Path(temp)
        unavailable = base / "unavailable.c"
        unavailable.write_text('#include "build_manifest.h"\nint main(void) { return psx_compiled_build_manifest_sha256()[0] != 0 || psx_compiled_build_manifest_schema() != 0; }\n')
        unavailable_exe = base / "unavailable.exe"
        run([args.cc, str(unavailable), "-I" + str(sdk / "runtime/include"), "-o", str(unavailable_exe)])
        run([str(unavailable_exe)])
        source = base / "game"
        source.mkdir()
        (source / "src").mkdir()
        (source / "generated").mkdir()
        (source / "src/plugin.h").write_text("#define PLUGIN_FACTOR 2\n")
        (source / "src/plugin.c").write_text('#include "plugin.h"\nint plugin(void) { return PLUGIN_FACTOR + PLUGIN_OPTION; }\n')
        (source / "src/mixed.cpp").write_text('#ifndef LANGUAGE_CXX\n#error missing CXX options\n#endif\nextern "C" int cpp_marker(void) { return 3; }\n')
        (source / "src/object.c").write_text('int object_marker(void) { return 7; }\n')
        (source / "generated/game.c").write_text('int generated(void) { return 11; }\n')
        (source / "generated/reference.c").write_text('int selected(void) { return 5; }\n')
        (source / "generated/enhanced.c").write_text('int selected(void) { return 5; }\n')
        (source / "src/main.c").write_text('''#include "build_manifest.h"
#include <stdio.h>
extern int plugin(void), generated(void), selected(void), cpp_marker(void);
int main(void) {
    printf("same-title-v10 %s %u %d\\n", psx_compiled_build_manifest_sha256(),
           psx_compiled_build_manifest_schema(), plugin()+generated()+selected()+cpp_marker());
    return 0;
}
''')
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(manifest_fixture C CXX)
include("{sdk.as_posix()}/cmake/psx_execution_profile.cmake")
include("{sdk.as_posix()}/cmake/psx_build_manifest.cmake")
set(PLUGIN_OPTION 1 CACHE STRING "fixture option")
add_executable(fixture src/main.c)
target_include_directories(fixture PRIVATE "{sdk.as_posix()}/runtime/include")
psxrecomp_build_manifest(fixture
    ROOTS "game=${{CMAKE_CURRENT_SOURCE_DIR}}" "build=${{CMAKE_CURRENT_BINARY_DIR}}" "sdk={sdk.as_posix()}"
    INPUTS "${{CMAKE_CURRENT_SOURCE_DIR}}/CMakeLists.txt"
    HEADER_DIRECTORIES "${{CMAKE_CURRENT_SOURCE_DIR}}/src"
    DEPENDENCY_IDENTITIES "net=e53059306824edf4f0aec4aae5ae65151b18535f")
# Registration must capture later title target additions and actual flags.
target_sources(fixture PRIVATE src/plugin.c src/mixed.cpp generated/game.c)
add_library(resource_object OBJECT src/object.c)
target_sources(fixture PRIVATE $<TARGET_OBJECTS:resource_object>)
target_compile_definitions(fixture PRIVATE PLUGIN_OPTION=${{PLUGIN_OPTION}})
target_compile_definitions(fixture PRIVATE "$<$<COMPILE_LANGUAGE:C>:LANGUAGE_C=1>" "$<$<COMPILE_LANGUAGE:CXX>:LANGUAGE_CXX=1>")
set_source_files_properties("${{CMAKE_CURRENT_SOURCE_DIR}}/generated/reference.c" PROPERTIES GENERATED TRUE)
psxrecomp_execution_profile(fixture PROFILE REFERENCE)
psxrecomp_add_implementation(fixture NAME integer CONTRACT integer-v1
    HLE_SOURCES generated/enhanced.c LLE_SOURCES generated/reference.c)
''')

        def configure(src, build, option=1):
            command = [args.cmake, "-S", str(src), "-B", str(build), "-G", "Ninja",
                 "-DCMAKE_C_COMPILER=" + args.cc, "-DCMAKE_MAKE_PROGRAM=" + args.ninja,
                 "-DPLUGIN_OPTION=" + str(option)]
            if args.cxx:
                command.append("-DCMAKE_CXX_COMPILER=" + args.cxx)
            run(command)

        def build_read(build):
            run([args.cmake, "--build", str(build), "--parallel", "2"])
            exe = build / ("fixture.exe" if (build / "fixture.exe").exists() else "fixture")
            values = run([str(exe)]).strip().split()
            manifest = json.loads((build / "psx_build_manifest/fixture/manifest.json").read_text())
            assert values[:1] == ["same-title-v10"] and values[2] == "1", values
            assert len(values[1]) == 64 and values[1] == manifest["sha256"]
            assert not manifest["whole_binary"] and not manifest["heterogeneous_qualified"]
            languages = manifest["configuration_by_language"]
            assert {"C", "CXX"} <= set(languages), languages.keys()
            assert "LANGUAGE_C=1" in languages["C"]["target.COMPILE_DEFINITIONS"]
            assert "LANGUAGE_CXX=1" in languages["CXX"]["target.COMPILE_DEFINITIONS"]
            assert "game/generated/reference.c" in manifest["files"]
            assert "game/generated/enhanced.c" not in manifest["files"]
            return values[1], int(values[3])

        build = base / "build"
        configure(source, build)
        first = build_read(build)
        assert build_read(build) == first, "no-op rebuild drift"
        (source / "src/plugin.c").write_text('#include "plugin.h"\nint plugin(void) { return PLUGIN_FACTOR + PLUGIN_OPTION + 1; }\n')
        changed = build_read(build)
        assert changed[0] != first[0] and changed[1] == first[1] + 1, (first, changed)
        (source / "src/plugin.h").write_text("#define PLUGIN_FACTOR 3\n")
        header = build_read(build)
        assert header[0] != changed[0] and header[1] == changed[1] + 1
        configure(source, build, option=2)
        option = build_read(build)
        assert option[0] != header[0] and option[1] == header[1] + 1
        (source / "generated/game.c").write_text('int generated(void) { return 12; }\n')
        generated = build_read(build)
        assert generated[0] != option[0] and generated[1] == option[1] + 1
        # Retain an identical compatibility ID across every perturbation.
        relocated = base / "relocated/game"
        shutil.copytree(source, relocated)
        moved_build = base / "relocated/build"
        configure(relocated, moved_build, option=2)
        assert build_read(moved_build) == generated, "logical-root relocation drift"
        (source / "src/new.inc").write_text("/* additional trusted header */\n")
        assert build_read(build)[0] != generated[0], "new header not noticed"
        # Missing/unmapped inputs cannot silently weaken the manifest.
        with (source / "CMakeLists.txt").open("a") as stream:
            stream.write('\nset_property(TARGET fixture APPEND PROPERTY PSX_BUILD_MANIFEST_INPUTS "' +
                         (base / 'outside.h').as_posix() + '")\n')
        (base / "outside.h").write_text("outside\n")
        configure(source, build, option=2)
        failure = run([args.cmake, "--build", str(build), "--parallel", "2"], ok=False)
        assert "unmapped manifest input" in failure
    print("build manifest: compiled API, same-ID implementation/header/options/generated changes, no-op, relocation, new-header and unmapped-input rejection passed")


if __name__ == "__main__":
    main()
