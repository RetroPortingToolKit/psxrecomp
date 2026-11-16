#!/usr/bin/env python3
"""The overlay codegen hash must not depend on line endings.

runtime/hash_codegen.cmake is the one place that computes
PSX_OVERLAY_CODEGEN_HASH. Everything else reads the header it writes
(overlay_api.h, compile_overlays.codegen_hash, psxrecomp-game --codegen-hash,
the boot-state header, the packagers). The value names the overlay cache
folder and is compared across hosts, so a Windows, a Linux and a macOS build
of one source revision must compute the same number.

Three of the hashed sources are stored with CRLF in git. The script reads each
file with CMake's file(READ), which returns LF line ends for a CRLF file, so
the value is the hash of the LF text on every host (Windows and Linux builds
of one revision agree; hashing the raw CRLF blobs would give a different
value, which no build uses). Nothing in the script says so, and a rewrite
that hashed raw bytes would split the cache folder name by host. This test
holds the property.

This test:
  1. hashes an LF copy and a CRLF copy of one set of sources and requires the
     same value;
  2. fixes the definition with a Python reference (SHA-256 over the sources,
     concatenated in list order, CRLF read as LF; first 8 hex digits);
  3. checks the real source list of this tree against that reference, so a
     host whose CMake reads a file differently fails here.
"""

from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "runtime" / "hash_codegen.cmake"
SOURCE_LIST = ROOT / "runtime" / "codegen_hash_sources.cmake"


def reference_hash(paths: list[Path]) -> int:
    digest = hashlib.sha256()
    for path in paths:
        if path.exists():
            digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return int(digest.hexdigest()[:8], 16)


def cmake_hash(cmake: str, paths: list[Path], out: Path) -> int:
    result = subprocess.run(
        [cmake, f"-DOUT={out.as_posix()}",
         "-DSRCS=" + ";".join(p.as_posix() for p in paths),
         "-P", str(SCRIPT)],
        text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"hash_codegen.cmake failed:\n{result.stderr}")
    match = re.search(r"#define\s+PSX_OVERLAY_CODEGEN_HASH\s+0x([0-9A-Fa-f]{8})u",
                      out.read_text(encoding="utf-8"))
    assert match, f"no hash in {out}"
    return int(match.group(1), 16)


def real_sources() -> list[Path]:
    text = SOURCE_LIST.read_text(encoding="utf-8")
    names = re.findall(r"\$\{PSXRECOMP_CODEGEN_HASH_ROOT\}/(\S+?)\)?\s*$", text,
                       flags=re.MULTILINE)
    paths = [ROOT / name for name in names]
    assert len(paths) >= 20, f"source list parse found only {len(paths)} files"
    missing = [str(p) for p in paths if not p.exists()]
    assert not missing, f"listed hash sources do not exist: {missing}"
    return paths


def main() -> int:
    cmake = shutil.which("cmake")
    if not cmake:
        print("SKIP: cmake not on PATH")
        return 0

    with tempfile.TemporaryDirectory(prefix="psx-cghash-") as raw:
        tmp = Path(raw)
        bodies = [
            "int a(void) {\n    return 1;\n}\n",
            "/* second file */\n\n#define B 2\n",
            "last line has no newline",
        ]
        variants = {}
        for name, newline in (("lf", "\n"), ("crlf", "\r\n")):
            folder = tmp / name
            folder.mkdir()
            paths = []
            for index, body in enumerate(bodies):
                path = folder / f"src{index}.c"
                path.write_bytes(body.replace("\n", newline).encode("utf-8"))
                paths.append(path)
            variants[name] = paths
        # One CRLF file among LF files, as in the real tree.
        mixed = tmp / "mixed"
        mixed.mkdir()
        mixed_paths = []
        for index, body in enumerate(bodies):
            path = mixed / f"src{index}.c"
            data = body.replace("\n", "\r\n" if index == 1 else "\n")
            path.write_bytes(data.encode("utf-8"))
            mixed_paths.append(path)
        variants["mixed"] = mixed_paths

        values = {name: cmake_hash(cmake, paths, tmp / f"{name}.h")
                  for name, paths in variants.items()}
        references = {name: reference_hash(paths)
                      for name, paths in variants.items()}
        assert len(set(values.values())) == 1, (
            "codegen hash depends on line endings: "
            + ", ".join(f"{k}=0x{v:08x}" for k, v in values.items()))
        assert values == references, (values, references)

        # The LF bytes alone, hashed as they are, give the same number: reading
        # CRLF as LF is the whole normalisation.
        raw_lf = hashlib.sha256(
            b"".join(p.read_bytes() for p in variants["lf"])).hexdigest()[:8]
        assert values["lf"] == int(raw_lf, 16)

        # Content still counts.
        changed = tmp / "changed"
        changed.mkdir()
        changed_paths = []
        for index, body in enumerate(bodies):
            path = changed / f"src{index}.c"
            path.write_bytes((body + ("x" if index == 0 else "")).encode("utf-8"))
            changed_paths.append(path)
        assert cmake_hash(cmake, changed_paths, tmp / "changed.h") != values["lf"]

        sources = real_sources()
        crlf = [p.name for p in sources if b"\r\n" in p.read_bytes()]
        tree = cmake_hash(cmake, sources, tmp / "tree.h")
        expect = reference_hash(sources)
        assert tree == expect, (
            f"this host computes 0x{tree:08x} for the tree; the line-ending-"
            f"proof reference is 0x{expect:08x}")
        print(f"codegen hash line-ending test: PASS "
              f"(tree 0x{tree:08x}, {len(sources)} sources, "
              f"{len(crlf)} with CRLF in this checkout: {', '.join(crlf) or 'none'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
