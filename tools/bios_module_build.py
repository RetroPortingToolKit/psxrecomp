#!/usr/bin/env python3
"""bios_module_build.py — build a loadable BIOS backend from the player's own
retail dump, on the player's machine.

WHY
===
A bundled release links only the redistributable OpenBIOS backend: the C a
retail image recompiles to is a translation of that image's code and never
ships (docs/BIOS_SELECTION.md). This script is what the runtime runs, from the
overlay toolchain beside the executable, when the player selects a retail dump:
it recompiles THAT dump with the bundled psxrecomp-bios and compiles the result
into a shared library the runtime loads as one more backend
(runtime/src/psx_bios_module.c, runtime/include/psx_bios_module.h). The same
interpreter and compiler already turn overlays streamed from the player's disc
into native code; this is that mechanism applied to the BIOS.

CONTRACT
========
The module is a self-contained shared library (no imports from the executable
on any platform), exporting psx_bios_module_abi / _codegen_hash / _init /
_backend (see psx_bios_module_glue.c.inc). It is compiled with the same flags
the executable's own BIOS backend uses (no debug tools, block cycles, game
dispatch present) plus the two module defines, from THREE translation units:

    <STEM>_full.c       generated function bodies      (+ psx_bios_module.h prepended)
    <STEM>_dispatch.c   generated dispatcher/descriptor (+ psx_bios_module.h prepended)
    glue.c              overlay preamble + BIOS glue    (the only exports)

The identity gate is the emitter's own: a profile pins the image's SHA-256 and
psxrecomp-bios refuses any other image, so a module can only ever be built from
the exact dump its code describes.

USAGE (as the runtime invokes it)
    python3 bios_module_build.py --dump <SCPH1001.BIN> --toolchain <dir> \
        --stem SCPH1001 --out <cache>/SCPH1001_<crc8>.so --flavor 0 \
        --compiler gcc|tcc [--gcc <cc>] [--tcc <tcc>] [--arch-abi <os>-<arch>]

The toolchain dir is overlay_toolchain/ as tools/release_stage.py stages it
(python, psxrecomp-bios, include/, bios/*.toml, recompiler/seeds/). Each
piece can be overridden (--emitter, --include, --profiles, --seeds) so a
developer tree can run it without staging.

Prints `PSX_BIOS_MODULE_PUBLISHED <path>` on success; exit 0. Any failure
leaves no file at --out and exits non-zero with the reason on stdout.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import compile_overlays as co  # noqa: E402  (shared compiler driver helpers)


def die(msg: str) -> "NoReturn":
    print(f"bios_module_build: ERROR: {msg}")
    sys.exit(1)


def read_profile(path: str) -> dict:
    """The few keys this needs from a bios/<stem>.toml (simple `key = "value"`
    lines inside [program] / [program.image] / [recompiler]). No TOML library:
    the pinned interpreter is the stdlib-only embeddable Python."""
    out = {"rom": "", "sha256": "", "seeds": "", "out_stem": "", "id": ""}
    section = ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.split("#", 1)[0].strip()
            if not s:
                continue
            m = re.match(r"^\[([^\]]+)\]$", s)
            if m:
                section = m.group(1).strip()
                continue
            m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"([^"]*)"', s)
            if not m:
                continue
            k, v = m.group(1), m.group(2)
            if section == "program" and k == "rom":
                out["rom"] = v
            elif section == "program" and k == "id":
                out["id"] = v
            elif section == "program.image" and k == "sha256":
                out["sha256"] = v.lower()
            elif section == "recompiler" and k == "seeds":
                out["seeds"] = v
            elif section == "recompiler" and k == "out_stem":
                out["out_stem"] = v
    return out


def identity(path: str) -> tuple[int, int, str]:
    with open(path, "rb") as f:
        data = f.read()
    return len(data), zlib.crc32(data) & 0xFFFFFFFF, hashlib.sha256(data).hexdigest()


MODULE_DEFINES = [
    "-DPSX_OVERLAY_DLL_BUILD",
    "-DPSX_BIOS_MODULE_BUILD",
    # Mirror the executable's own BIOS objects (runtime.cmake): no debug tools,
    # faithful block cycles, and the game-dispatch-aware dispatcher body.
    "-DPSX_NO_DEBUG_TOOLS",
    "-DPSX_ENABLE_BLOCK_CYCLES=1",
    "-DPSX_HAS_GAME_DISPATCH",
]


def compile_module(sources: list[str], out: str, include_dirs: list[str],
                   stem: str, flavor: int, compiler: str, gcc: str, tcc: str) -> bool:
    defines = MODULE_DEFINES + [f"-DPSX_OVERLAY_FLAVOR={int(flavor)}",
                                f"-DPSX_BIOS_MODULE_STEM={stem}"]
    if int(flavor) & 2:
        defines.append("-DPSX_PGXP=1")
    if compiler == "tcc":
        # Same treatment compile_overlays gives a shard: strip any BOM, add the
        # __builtin_ctz shim, and hand tcc BOM-free copies of the headers.
        for src in sources:
            with open(src, "rb") as f:
                data = f.read()
            if data[:3] == b"\xef\xbb\xbf":
                data = data[3:]
            if not data.startswith(co._TCC_COMPAT_PREFIX):
                data = co._TCC_COMPAT_PREFIX + data
            with open(src, "wb") as f:
                f.write(data)
        cmd = [tcc, "-shared", *defines, *[co.native_path(s) for s in sources],
               "-o", co.native_path(out)]
        for d in include_dirs:
            cmd.append("-I" + co.native_path(co._bom_free_incdir(d)))
        env = None
    else:
        env, tc_dir = co._toolchain_env(gcc)
        pic = [] if co.is_windows() else ["-fPIC"]
        cmd = [gcc, "-shared", *pic, *co.target_arch_flags("gcc"), "-O2", *defines,
               *[co.native_path(s) for s in sources],
               "-o", co.native_path(out),
               *[f"-I{co.native_path(d)}" for d in include_dirs], "-lm"]
        if tc_dir is None:
            print("  WARNING: no gcc toolchain dir found; compile may fail to launch")
    print("  compile: " + " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        msg = (r.stderr or r.stdout or "").strip() or "(no compiler output)"
        print(f"  COMPILE ERROR (exit {r.returncode}):\n{msg}")
        return False
    return True


def self_check(path: str, flavor: int) -> bool:
    """Load the fresh module in this interpreter: proves it is self-contained
    (RTLD_NOW resolves every symbol or fails) and that its ABI tag names the
    flavor asked for. The host repeats the gate with its own tag."""
    try:
        lib = ctypes.CDLL(co.native_path(path))
    except OSError as exc:
        print(f"  self-check: module does not load: {exc}")
        return False
    try:
        abi = ctypes.c_int(lib.psx_bios_module_abi()).value
        h = ctypes.c_uint32(lib.psx_bios_module_codegen_hash()).value
    except AttributeError as exc:
        print(f"  self-check: export missing: {exc}")
        return False
    if (abi >> 16) & 0xFFFF != int(flavor) & 0xFFFF:
        print(f"  self-check: ABI flavor {(abi >> 16) & 0xFFFF} != requested {flavor}")
        return False
    print(f"  self-check: abi=0x{abi:08x} codegen_hash=0x{h:08x} ok")
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dump", required=True, help="the player's BIOS image")
    ap.add_argument("--toolchain", default=HERE, help="overlay_toolchain/ dir (default: this script's dir)")
    ap.add_argument("--stem", required=True, help="profile stem, e.g. SCPH1001 (bios/<stem>.toml)")
    ap.add_argument("--out", required=True, help="module path to publish (.so/.dll)")
    ap.add_argument("--flavor", type=int, default=0, help="PSX_OVERLAY_FLAVOR of the host build")
    ap.add_argument("--arch-abi", default=None,
                    help="\"<os>-<arch>\" of the host build (overlay_loader.h's PSX_OVERLAY_ARCH_ABI); "
                         "default: PSX_OVERLAY_ARCH_ABI from the environment, else this interpreter's")
    ap.add_argument("--compiler", choices=("gcc", "tcc"), default="gcc")
    ap.add_argument("--gcc", default="gcc")
    ap.add_argument("--tcc", default="tcc")
    ap.add_argument("--emitter", default=None, help="psxrecomp-bios (default: <toolchain>/psxrecomp-bios[.exe])")
    ap.add_argument("--include", default=None, help="runtime include dir (default: <toolchain>/include)")
    ap.add_argument("--profiles", default=None, help="bios/ profiles dir (default: <toolchain>/bios)")
    ap.add_argument("--seeds", default=None, help="seeds dir (default: <toolchain>/recompiler/seeds)")
    ap.add_argument("--keep-work", action="store_true", help="keep the temporary emit tree (debugging)")
    args = ap.parse_args(argv)

    # Build for the HOST's architecture, which on macOS need not be the
    # compiler binary's default (an x86_64 runtime under Rosetta, arm64-only
    # Xcode clang). The self-check loads the module into this interpreter, so
    # the interpreter has to run as that architecture as well.
    env_arch = os.environ.get("PSX_OVERLAY_ARCH_ABI")
    if env_arch and args.arch_abi and env_arch != args.arch_abi:
        print(f"PSX_OVERLAY_ARCH_ABI overrides --arch-abi: {env_arch}")
    arch_err = co.apply_runtime_arch_abi(
        env_arch or args.arch_abi,
        "PSX_OVERLAY_ARCH_ABI" if env_arch else "--arch-abi")
    if arch_err:
        die(arch_err)
    arch_mismatch = co.interpreter_arch_mismatch()
    if arch_mismatch:
        die(f"cannot build a BIOS module: {arch_mismatch}")

    tk = os.path.abspath(args.toolchain)
    # Everything absolute: the emitter runs with cwd inside the staged work
    # tree, and the compiler is handed paths from more than one directory.
    emitter = os.path.abspath(args.emitter or os.path.join(
        tk, "psxrecomp-bios.exe" if co.is_windows() else "psxrecomp-bios"))
    include = os.path.abspath(args.include or os.path.join(tk, "include"))
    profiles = os.path.abspath(args.profiles or os.path.join(tk, "bios"))
    seeds_dir = os.path.abspath(args.seeds or os.path.join(tk, "recompiler", "seeds"))
    args.dump = os.path.abspath(args.dump)
    args.out = os.path.abspath(args.out)
    for label, p in (("emitter", emitter), ("include dir", include), ("profiles dir", profiles),
                     ("seeds dir", seeds_dir), ("dump", args.dump)):
        if not os.path.exists(p):
            die(f"{label} not found: {p}")
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", args.stem):
        die(f"bad stem {args.stem!r}")
    profile_path = os.path.join(profiles, f"{args.stem}.toml")
    if not os.path.isfile(profile_path):
        die(f"no profile for stem {args.stem}: {profile_path}")
    prof = read_profile(profile_path)
    if not prof["rom"] or not prof["seeds"]:
        die(f"profile {profile_path} lacks rom/seeds")
    out_stem = prof["out_stem"] or args.stem
    if out_stem != args.stem:
        die(f"profile out_stem {out_stem!r} != --stem {args.stem!r}; the descriptor symbol must match")

    size, crc, sha = identity(args.dump)
    print(f"dump: {args.dump} ({size} bytes, CRC32 {crc:08X}, SHA-256 {sha[:16]}…)")
    if size != 512 * 1024:
        die(f"a PlayStation BIOS image is exactly 524288 bytes (got {size})")
    if prof["sha256"] and prof["sha256"] != sha:
        die(f"this image is not {prof['id'] or args.stem}: profile pins SHA-256 "
            f"{prof['sha256'][:16]}…, dump is {sha[:16]}…")

    seeds_src = os.path.join(seeds_dir, os.path.basename(prof["seeds"]))
    if not os.path.isfile(seeds_src):
        die(f"seeds file the profile names is not shipped: {seeds_src}")

    # Stage an emitter project root: the profile's rom/seeds paths are relative
    # to the directory holding a .gitignore/.git/CMakeLists.txt marker
    # (config_loader find_project_root), so recreate exactly that shape.
    work = tempfile.mkdtemp(prefix="psx_bios_module_")
    try:
        with open(os.path.join(work, ".gitignore"), "w") as f:
            f.write("*\n")
        rom_dst = os.path.join(work, prof["rom"])
        os.makedirs(os.path.dirname(rom_dst), exist_ok=True)
        shutil.copy2(args.dump, rom_dst)
        seeds_dst = os.path.join(work, prof["seeds"])
        os.makedirs(os.path.dirname(seeds_dst), exist_ok=True)
        shutil.copy2(seeds_src, seeds_dst)
        prof_dst = os.path.join(work, "bios", f"{args.stem}.toml")
        os.makedirs(os.path.dirname(prof_dst), exist_ok=True)
        shutil.copy2(profile_path, prof_dst)
        gen = os.path.join(work, "out")
        os.makedirs(gen, exist_ok=True)

        env = os.environ.copy()
        env.setdefault("PSX_CPS", "1")
        cmd = [emitter, "--config", os.path.join("bios", f"{args.stem}.toml"), "--out-dir", gen]
        print("  emit: " + " ".join(cmd))
        r = subprocess.run(cmd, cwd=work, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            tail = "\n".join((r.stderr or r.stdout or "").splitlines()[-25:])
            die(f"psxrecomp-bios exited {r.returncode}:\n{tail}")
        full_c = os.path.join(gen, f"{args.stem}_full.c")
        disp_c = os.path.join(gen, f"{args.stem}_dispatch.c")
        for p in (full_c, disp_c):
            if not os.path.isfile(p):
                die(f"emitter produced no {p}")
        with open(disp_c, encoding="utf-8", errors="replace") as f:
            if f"{args.stem}_psx_bios_backend" not in f.read():
                die("emitted dispatch defines no backend descriptor (emitter too old)")

        # Module translation units: the name mappings go before the first
        # #include so psx_memory.h's g_psx_ram_mask is rewritten too.
        prelude = '#include "psx_bios_module.h"\n'
        for p in (full_c, disp_c):
            with open(p, "rb") as f:
                data = f.read()
            if data[:3] == b"\xef\xbb\xbf":
                data = data[3:]
            with open(p, "wb") as f:
                f.write(prelude.encode("utf-8") + data)
        glue_c = os.path.join(gen, "glue.c")
        with open(glue_c, "w", encoding="utf-8") as f:
            f.write(prelude)
            f.write('#include "overlay_dispatch_preamble.c.inc"\n')
            f.write('#include "psx_bios_module_glue.c.inc"\n')

        out_dir = os.path.dirname(os.path.abspath(args.out)) or "."
        os.makedirs(out_dir, exist_ok=True)
        tmp_out = os.path.join(out_dir, "." + os.path.basename(args.out) + ".tmp")
        if os.path.exists(tmp_out):
            os.remove(tmp_out)
        include_dirs = [include]
        fmt_inc = os.path.join(tk, "lib", "fmt", "include")   # dev-tree layout only
        if os.path.isdir(fmt_inc):
            include_dirs.append(fmt_inc)
        if not compile_module([full_c, disp_c, glue_c], tmp_out, include_dirs,
                              args.stem, args.flavor, args.compiler, args.gcc, args.tcc):
            if os.path.exists(tmp_out):
                os.remove(tmp_out)
            die("compile failed")
        if not self_check(tmp_out, args.flavor):
            os.remove(tmp_out)
            die("self-check failed")
        os.replace(tmp_out, args.out)
        print(f"PSX_BIOS_MODULE_PUBLISHED {args.out}")
        return 0
    finally:
        if args.keep_work:
            print(f"  work tree kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
