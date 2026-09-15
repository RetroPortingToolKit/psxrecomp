"""Release must retain fixture assertions without changing production flags."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--build-tool", required=True)
    args = parser.parse_args()
    module = Path(__file__).resolve().parents[1] / "test_assertions.cmake"
    with tempfile.TemporaryDirectory(prefix="psx-test-assertions-") as tmp:
        root = Path(tmp)
        (root / "tests").mkdir()
        (root / "nested").mkdir()
        (root / "tests/probe.c").write_text(
            '#include <assert.h>\n#ifdef NDEBUG\n#error Assertions disabled\n#endif\n'
            'int main(void) { int observed=0; assert(++observed==1); return observed!=1; }\n')
        (root / "production.c").write_text(
            '#ifndef NDEBUG\n#error Production flags changed\n#endif\nint main(void) { return 0; }\n')
        (root / "nested/CMakeLists.txt").write_text(
            'add_executable(nested_probe "${CMAKE_CURRENT_SOURCE_DIR}/../tests/probe.c")\n'
            'add_test(NAME nested_probe COMMAND nested_probe)\n')
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(assertions C)\nenable_testing()\n'
            'add_executable(probe tests/probe.c)\nadd_test(NAME probe COMMAND probe)\n'
            'add_executable(production production.c)\nadd_subdirectory(nested)\n'
            f'include("{module.as_posix()}")\npsxrecomp_enable_test_assertions()\n')
        build = root / "build"
        subprocess.run([args.cmake, "-S", str(root), "-B", str(build), "-G", args.generator,
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DCMAKE_C_COMPILER=" + args.cc,
                        "-DCMAKE_MAKE_PROGRAM=" + args.build_tool], check=True)
        subprocess.run([args.cmake, "--build", str(build), "--config", "Release",
                        "--target", "psxrecomp-test-fixtures"], check=True)
        suffix = ".exe" if os.name == "nt" else ""
        if list(build.rglob("production" + suffix)):
            raise RuntimeError("Fixture aggregate unexpectedly built production")
        for name in ("probe", "nested_probe"):
            executables = list(build.rglob(name + suffix))
            if len(executables) != 1:
                raise RuntimeError(f"Expected one {name} executable, found {executables}")
            subprocess.run([str(executables[0])], check=True)
        subprocess.run([args.cmake, "--build", str(build), "--config", "Release",
                        "--target", "production"], check=True)
    print("Release fixture assertions retained; aggregate excludes production; production flags unchanged")


if __name__ == "__main__":
    main()
