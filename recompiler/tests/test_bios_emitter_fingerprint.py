#!/usr/bin/env python3
"""The BIOS emitter fingerprint hashes what psxrecomp-bios is built from.

tools/bios_emitter_fingerprint.cmake decides whether a committed
generated/<stem>.emitter.sha is stale. If it hashes the wrong files, an
emitter change can leave the stamp unchanged, or an unrelated change can make
it move. This test holds the script to the target, in both directions, with a
second reading of the same files written here in Python:

  1. The fingerprint equals the documented digest of the target's sources,
     the local headers they include, the cycle headers and the profile inputs.
  2. Its source list is the target's source list, in order: none missing,
     none extra. With --target-sources, CMake's own list of the target's
     sources is the reference.
  3. Its header list is the quoted-include closure of those sources.
  4. On a copy of the tree: a change to each kind of input moves the
     fingerprint, a change to a file the target does not compile does not, a
     source added to the target is hashed with no edit of the script, and a
     target the script cannot read is an error, not an empty list.

It needs only cmake, not a configured build tree.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = "tools/bios_emitter_fingerprint.cmake"
CMAKE_LISTS = "recompiler/CMakeLists.txt"
TARGET = "psxrecomp-bios"
INCLUDE_DIRS = ("recompiler/include", "recompiler/src")
EXTRAS = ("runtime/include/psx_cyc.h", "runtime/include/psx_instr_cost.h")
PROFILE = "bios/OpenBIOS.toml"
# Sources the target compiles, and files it does not compile that sit beside them.
TARGET_ONLY = ("recompiler/src/ps1_exe_parser.cpp", "recompiler/src/recompiler_patch.cpp")
NOT_IN_TARGET = ("recompiler/src/basic_block.cpp", "recompiler/src/control_flow.cpp",
                 "recompiler/src/function_analysis.cpp")


class Tree:
    """One tree with the script in it: the checkout, or a copy of the files it needs."""

    def __init__(self, root: Path, cmake: str):
        self.root = root
        self.cmake = cmake

    def run(self, profile: str, *defines: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.cmake, f"-DROOT={self.root.as_posix()}", f"-DPROFILE={profile}", *defines,
             "-P", str(self.root / SCRIPT)],
            cwd=self.root, stdin=subprocess.DEVNULL, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120)

    def fingerprint(self, profile: str = PROFILE) -> str:
        result = self.run(profile)
        assert result.returncode == 0, (profile, result.returncode, result.stderr)
        value = result.stdout.strip().splitlines()[-1].strip()
        assert re.fullmatch(r"[0-9a-f]{64}", value), (profile, result.stdout, result.stderr)
        return value

    def listing(self, profile: str = PROFILE) -> list[tuple[str, str, str]]:
        result = self.run(profile, "-DFP_LIST=1")
        assert result.returncode == 0, (profile, result.returncode, result.stderr)
        rows = [tuple(line.split("\t")) for line in result.stdout.splitlines() if line.strip()]
        assert rows and all(len(row) == 3 for row in rows), (
            "the script does not list its inputs (-DFP_LIST=1):\n" + result.stdout)
        return rows

    def of_kind(self, kind: str, profile: str = PROFILE) -> list[str]:
        return [path for row_kind, _, path in self.listing(profile) if row_kind == kind]


def target_sources(root: Path) -> list[str]:
    """Second reading of add_executable(psxrecomp-bios ...), paths from the repository root."""
    text = (root / CMAKE_LISTS).read_text(encoding="utf-8")
    text = re.sub(r"#[^\n]*", "", text)
    block = re.search(r"add_executable\s*\(\s*" + re.escape(TARGET) + r"(?=[\s)])([^)]*)\)", text)
    assert block, f"{CMAKE_LISTS} has no add_executable({TARGET} ...)"
    return [os.path.normpath("recompiler/" + word).replace(os.sep, "/") for word in block.group(1).split()]


def include_closure(root: Path, sources: list[str]) -> list[str]:
    """Second reading of the headers: quoted includes, followed through the repository."""
    seen = list(sources)
    headers = []
    for file in seen:                      # grows while it is walked
        text = (root / file).read_text(encoding="utf-8", errors="replace")
        for name in re.findall(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', text, re.M):
            for base in (os.path.dirname(file), *INCLUDE_DIRS):
                candidate = os.path.normpath(os.path.join(base, name)).replace(os.sep, "/")
                if candidate.startswith(".."):
                    continue
                if (root / candidate).is_file():
                    if candidate not in seen:
                        seen.append(candidate)
                        headers.append(candidate)
                    break
    return sorted(headers)


def profile_inputs(root: Path, profile: str) -> list[str]:
    text = (root / profile).read_text(encoding="utf-8")
    seeds = re.search(r'^[ \t]*seeds[ \t]*=[ \t]*"([^"]*)"', text, re.M)
    return [profile] + ([seeds.group(1)] if seeds and seeds.group(1) else [])


def expected_inputs(root: Path, profile: str, sources: list[str]) -> list[str]:
    headers = include_closure(root, sources)
    extras = [name for name in EXTRAS if name not in headers]
    return [*sources, *headers, *extras, *profile_inputs(root, profile)]


def documented_digest(root: Path, files: list[str]) -> str:
    """The script's digest, computed here: one line per existing file, CRs stripped."""
    lines = "".join(hashlib.sha256((root / f).read_bytes().replace(b"\r", b"")).hexdigest() + "  -\n"
                    for f in files if (root / f).is_file())
    return hashlib.sha256(lines.encode()).hexdigest()


def two_way(what: str, wanted: list[str], hashed: list[str]) -> None:
    """Fail unless the two lists hold the same files, and print both sides."""
    missing = sorted(set(wanted) - set(hashed))
    extra = sorted(set(hashed) - set(wanted))
    print(f"{what}: {len(wanted)} wanted, {len(hashed)} fingerprinted, "
          f"{len(missing)} not fingerprinted, {len(extra)} fingerprinted without cause")
    assert wanted, f"{what}: the wanted list is empty; nothing was examined"
    assert not missing and not extra, (
        f"{what}: the fingerprint list and its source disagree\n"
        f"  in the source, not fingerprinted: {missing}\n"
        f"  fingerprinted, not in the source: {extra}")


def check_lists(tree: Tree, cmake_sources: str | None) -> None:
    target = target_sources(tree.root)
    if cmake_sources is not None:
        # Generated entries (the Windows manifest object CMake adds to the
        # target) are absolute paths in the build tree, not sources.
        from_cmake = [os.path.normpath("recompiler/" + word).replace(os.sep, "/")
                      for word in cmake_sources.split(",")
                      if word and not os.path.isabs(word)]
        two_way("target sources: CMake's own list against this test's reading", from_cmake, target)
        target = from_cmake
    else:
        print("target sources: read from the CMake text only (no --target-sources)")

    wanted = expected_inputs(tree.root, PROFILE, target)
    assert tree.fingerprint() == documented_digest(tree.root, wanted), (
        "the fingerprint is not the digest of the psxrecomp-bios sources, their included "
        "headers, the cycle headers and the profile inputs")
    print(f"fingerprint: equals the digest of {len(wanted)} target inputs")

    rows = tree.listing()
    paths = [path for _, _, path in rows]
    assert paths == wanted, f"the inputs are not hashed in the documented order: {paths}"
    assert len(set(paths)) == len(paths), f"a file is fingerprinted twice: {paths}"
    sources = tree.of_kind("source")
    two_way("emitter sources: psxrecomp-bios target against the fingerprint", target, sources)
    two_way("emitter headers: include lines against the fingerprint",
            include_closure(tree.root, target), tree.of_kind("header"))
    absent = [path for kind, state, path in rows if kind != "profile" and state != "present"]
    assert not absent, f"the fingerprint names emitter files that do not exist: {absent}"
    for name in TARGET_ONLY:
        assert name in sources, f"{name} is a source of the target and is not fingerprinted"
    for name in NOT_IN_TARGET:
        assert name not in paths, f"{name} is fingerprinted and is not in the target"


def copy_tree(source: Tree, dest: Path) -> Tree:
    """The files the script reads, and files it must not read, in a folder of their own."""
    emitter = [path for kind, _, path in source.listing() if kind != "profile"]
    for name in [SCRIPT, CMAKE_LISTS, *emitter, *NOT_IN_TARGET]:
        if (source.root / name).is_file():
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source.root / name, dest / name)
    (dest / "bios").mkdir()
    (dest / "bios" / "T.toml").write_text(
        '[recompiler]\nrom = "bios/T.BIN"\nseeds = "bios/T-seeds.json"\nout_stem = "T"\n',
        encoding="utf-8", newline="\n")
    (dest / "bios" / "T-seeds.json").write_bytes(b'{"seeds": []}\n')
    return Tree(dest, source.cmake)


def touch(path: Path) -> bytes:
    """Add one byte; return the original bytes to restore."""
    original = path.read_bytes()
    path.write_bytes(original + b"\n")
    return original


def check_behaviour(copy: Tree) -> None:
    profile = "bios/T.toml"
    base = copy.fingerprint(profile)
    header = copy.of_kind("header", profile)[0]

    moving = [*TARGET_ONLY, "recompiler/src/main_bios.cpp", header, "runtime/include/psx_cyc.h",
              profile, "bios/T-seeds.json"]
    for name in moving:
        original = touch(copy.root / name)
        assert copy.fingerprint(profile) != base, f"a change to {name} does not move the fingerprint"
        (copy.root / name).write_bytes(original)
    assert copy.fingerprint(profile) == base

    originals = {name: touch(copy.root / name) for name in NOT_IN_TARGET if (copy.root / name).is_file()}
    assert originals, "none of the files outside the target is in this tree"
    assert copy.fingerprint(profile) == base, f"a change to {sorted(originals)} moves the fingerprint"
    for name, original in originals.items():
        (copy.root / name).write_bytes(original)
    print(f"behaviour: {len(moving)} inputs move the fingerprint, "
          f"{len(originals)} files outside the target do not")

    # A source added to the target is hashed with no edit of the script.
    cmake = copy.root / CMAKE_LISTS
    original_cmake = cmake.read_text(encoding="utf-8")
    opening = re.search(r"add_executable\s*\(\s*" + re.escape(TARGET) + r"[ \t]*\r?\n", original_cmake)
    assert opening, "the copy's CMake text has no psxrecomp-bios block to extend"
    (copy.root / "recompiler/src/added_in_test.cpp").write_text('#include "added_in_test.h"\n', encoding="utf-8")
    (copy.root / "recompiler/src/added_in_test.h").write_text("// new header\n", encoding="utf-8")
    cmake.write_text(original_cmake[:opening.end()] + "    src/added_in_test.cpp  # with a comment\n"
                     + original_cmake[opening.end():], encoding="utf-8", newline="")
    assert "recompiler/src/added_in_test.cpp" in copy.of_kind("source", profile)
    assert "recompiler/src/added_in_test.h" in copy.of_kind("header", profile)
    assert copy.fingerprint(profile) != base, "a source added to the target is not hashed"

    # A target the script cannot read is an error. An empty list would hash
    # the profile alone and call every emitter change fresh.
    for label, text in (
            ("a variable in the target", original_cmake[:opening.end()] + "    ${BIOS_SOURCES}\n"
             + original_cmake[opening.end():]),
            ("no target", original_cmake.replace("add_executable(" + TARGET, "add_executable(some-other-tool"))):
        cmake.write_text(text, encoding="utf-8", newline="")
        result = copy.run(profile)
        assert result.returncode != 0 and not result.stdout.strip(), (label, result.returncode, result.stdout)
        assert "bios_emitter_fingerprint:" in result.stderr, (label, result.stderr)
    cmake.write_text(original_cmake, encoding="utf-8", newline="")
    os.remove(copy.root / "recompiler/src/added_in_test.cpp")
    os.remove(copy.root / "recompiler/src/added_in_test.h")
    assert copy.fingerprint(profile) == base
    print("behaviour: added source hashed without a script edit; unreadable target refused")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", help="the cmake program (default: cmake on PATH)")
    parser.add_argument("--target-sources",
                        help="the SOURCES of psxrecomp-bios as CMake holds them, joined by commas")
    args = parser.parse_args()
    cmake = args.cmake or shutil.which("cmake")
    assert cmake, "no cmake: pass --cmake"
    tree = Tree(ROOT, cmake)
    check_lists(tree, args.target_sources)
    with tempfile.TemporaryDirectory(prefix="psx-emitter-fp-") as tmp:
        check_behaviour(copy_tree(tree, Path(tmp)))
    print("bios emitter fingerprint test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
