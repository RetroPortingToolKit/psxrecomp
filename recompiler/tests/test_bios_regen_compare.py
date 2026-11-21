"""Exercise the canonical regeneration driver's comparison/failure contract.

The configured emitter below is a stand-in for process/file failure cases,
not a BIOS-generation correctness oracle. The real fingerprint implementation
and regeneration script run unchanged, with a real CMake/Ninja build invocation.
Available pinned BIOS inputs separately qualify actual generation.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cmake", "bash", "ninja"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--script", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        "bios_fingerprint_fixture", ROOT / "recompiler/tests/test_bios_emitter_fingerprint.py")
    fp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fp)
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bios compare ", dir=args.work_dir) as temporary:
        root = Path(temporary)
        tree = fp.copy_tree(fp.Tree(ROOT, args.cmake), root)
        for name in ("regen_bios.sh", "bios_emitter_fingerprint.sh"):
            source = args.script if name == "regen_bios.sh" and args.script else ROOT / "tools" / name
            shutil.copyfile(source, root / "tools" / name)
        profile = root / "bios/T.toml"
        profile.write_text(
            '[program]\nrom = "bios/T.BIN"\n[program.image]\nsha256 = "' + "a" * 64
            + '"\n[recompiler]\nseeds = "bios/T-seeds.json"\nout_dir = "generated"\nout_stem = "T"\n',
            encoding="utf-8", newline="\n")
        profile_original = profile.read_bytes()
        generated = root / "generated"
        generated.mkdir()
        baseline = root / "baseline"
        baseline.mkdir()
        names = ("T_full.c", "T_dispatch.c", "T.bios.c", "T.bios.h")
        for name in names:
            (baseline / name).write_text("/* " + name + " */\n", encoding="utf-8", newline="\n")
        build = root / "recompiler/fixture-build"
        build.mkdir()
        cache = build / "CMakeCache.txt"
        cache.write_text(
            "CMAKE_PROJECT_NAME:STATIC=BiosDriverFixture\nCMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_HOME_DIRECTORY:INTERNAL="
            + (root / "recompiler").as_posix() + "\nCMAKE_MAKE_PROGRAM:FILEPATH="
            + Path(args.ninja).as_posix() + "\n", encoding="utf-8", newline="\n")
        cache_original = cache.read_bytes()
        (build / "build.ninja").write_text(
            'rule observe\n  command = "' + Path(args.cmake).as_posix()
            + '" -E touch "' + (root / "built.marker").as_posix()
            + '"\nbuild fresh-marker: observe\nbuild psxrecomp-bios: phony fresh-marker\n',
            encoding="utf-8", newline="\n")
        emitter = build / "psxrecomp-bios"
        emitter.write_text(r"""#!/usr/bin/env bash
set -eu
fixture_root="$(cd "$(dirname "$0")/../.." && pwd)"
printf '%s\n' "$@" > "$fixture_root/invocation"
out=generated
while [ $# -gt 0 ]; do
    case "$1" in
        --config) shift 2 ;;
        --out-dir) out=$2; shift 2 ;;
        *) exit 20 ;;
    esac
done
mode=$(cat "$fixture_root/mode")
mkdir -p "$out"
if [ "$mode" = failed ]; then printf 'partial\n' > "$out/T_full.c"; exit 9; fi
for name in T_full.c T_dispatch.c T.bios.c T.bios.h; do
    cp "$fixture_root/baseline/$name" "$out/$name"
done
case "$mode" in
    changed) printf 'changed\n' > "$out/T_full.c" ;;
    removed) rm "$out/T.bios.h" ;;
    added) printf 'added\n' > "$out/T_extra.h" ;;
    unstable) printf '\n' >> "$fixture_root/recompiler/src/main_bios.cpp" ;;
esac
""", encoding="utf-8", newline="\n")
        emitter.chmod(0o755)
        temp = root / "scratch"
        temp.mkdir()
        env = os.environ.copy()
        for key in ("PSXRECOMP_BIOS_ROM", "PSXRECOMP_BIOS_SEEDS",
                    "PSXRECOMP_BIOS_OUT", "PSXRECOMP_BIOS_STEM"):
            env.pop(key, None)
        env.update(PSXRECOMP_BIOS_BUILD="recompiler/fixture-build",
                   TMPDIR=temp.as_posix(),
                   PATH=os.pathsep.join([str(Path(args.cmake).parent),
                                         str(Path(args.bash).parent), env["PATH"]]))
        stamp = generated / "T.emitter.sha"
        source = root / "recompiler/src/main_bios.cpp"
        source_original = source.read_bytes()
        previous = "0" * 64 + "\n"

        def reset(mode):
            source.write_bytes(source_original)
            profile.write_bytes(profile_original)
            cache.write_bytes(cache_original)
            for path in generated.iterdir():
                path.unlink()
            for name in names:
                shutil.copyfile(baseline / name, generated / name)
            stamp.write_text(previous, encoding="utf-8", newline="\n")
            (root / "mode").write_text(mode, encoding="utf-8")
            for name in ("built.marker", "invocation"):
                (root / name).unlink(missing_ok=True)

        def run(compare=True, extra_env=None):
            argv = [args.bash, str(root / "tools/regen_bios.sh")]
            if compare:
                argv += ["--compare"]
            argv += ["--config", "bios/T.toml"]
            result = subprocess.run(argv, cwd=root, env=dict(env, **(extra_env or {})),
                                    capture_output=True, text=True,
                                    encoding="utf-8", errors="replace")
            assert not list(temp.iterdir()), ("comparison scratch leaked", result.stdout, result.stderr)
            return result

        def unchanged():
            assert stamp.read_text(encoding="utf-8") == previous
            assert sorted(p.name for p in generated.iterdir()) == sorted((*names, stamp.name))
            for name in names:
                assert (generated / name).read_bytes() == (baseline / name).read_bytes()

        reset("equal")
        result = run()
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert "output-neutral (4 C/h files)" in result.stdout
        assert stamp.read_text(encoding="utf-8").strip() == tree.fingerprint("bios/T.toml")
        assert (root / "built.marker").exists()
        assert "--out-dir" in (root / "invocation").read_text()
        for mode in ("changed", "removed", "added"):
            reset(mode)
            result = run()
            assert result.returncode == 3, (mode, result.stdout, result.stderr)
            unchanged()
        for mode in ("failed", "unstable"):
            reset(mode)
            result = run()
            assert result.returncode == 4 and "unverified" in result.stderr, (
                mode, result.stdout, result.stderr)
            unchanged()
        reset("equal")
        profile.write_bytes(profile_original.replace(b"a" * 64, b""))
        result = run()
        assert result.returncode == 4 and not (root / "built.marker").exists()
        unchanged()
        reset("equal")
        other = root / "other"
        other.mkdir()
        cache.write_bytes(cache_original.replace(
            (root / "recompiler").as_posix().encode(), other.as_posix().encode()))
        result = run()
        assert result.returncode == 4 and not (root / "built.marker").exists()
        unchanged()
        reset("equal")
        result = run(extra_env={"PSXRECOMP_BIOS_OUT": "legacy-output"})
        assert result.returncode == 4 and not (root / "built.marker").exists()
        unchanged()
        reset("equal")
        profile.write_bytes(profile_original.replace(b'out_stem = "T"', b'out_stem = "../escaped"'))
        result = run()
        assert result.returncode == 4 and not (root / "invocation").exists()
        assert not (root / "escaped_full.c").exists()
        unchanged()
        reset("changed")
        result = run(compare=False)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert (generated / "T_full.c").read_text() == "changed\n"
        assert stamp.read_text(encoding="utf-8").strip() == tree.fingerprint("bios/T.toml")
        print("PASS: neutral refresh; changed/added/removed output; failed/unstable inputs; "
              "unpinned/foreign-build/legacy/escaping-stem refusal; normal regeneration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
