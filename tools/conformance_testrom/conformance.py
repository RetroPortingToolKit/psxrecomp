#!/usr/bin/env python3
"""Prepare and run pinned PS1 test-ROM fixtures against PSXRecomp."""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time


HERE = Path(__file__).resolve().parent
FIXTURES_PATH = HERE / "fixtures.json"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def require_hash(path: Path, expected: str) -> None:
    actual = sha256(path)
    if actual != expected.upper():
        raise RuntimeError(f"hash mismatch for {path}: {actual} != {expected}")


def run_checked(argv: list[str], cwd: Path) -> None:
    print("+", subprocess.list2cmdline(argv))
    subprocess.run(argv, cwd=cwd, check=True)


def normalized_log(data: bytes) -> str:
    text = data.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = text.splitlines()
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    lines = [line[2:] if line.startswith("% ") else line for line in lines]
    return "\n".join(lines) + ("\n" if lines else "")


def comparable_log(text: str) -> str:
    # Some ps1-tests binaries print a derived Total count that their pinned
    # expected log omits. Preserve it in actual_raw_normalized/result.json,
    # but do not let this non-assertion summary line decide conformance.
    lines = [line for line in text.splitlines()
             if not (line.startswith("Total tests: ") and line[13:].isdigit())]
    return "\n".join(lines) + ("\n" if lines else "")


def load_catalog() -> dict:
    return json.loads(FIXTURES_PATH.read_text(encoding="utf-8"))


def git_head(root: Path) -> str:
    return subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True
    ).strip()


def prepare(args: argparse.Namespace) -> int:
    catalog = load_catalog()
    spec = catalog["fixtures"].get(args.fixture)
    if spec is None:
        raise RuntimeError(f"unknown fixture: {args.fixture}")

    source_root = args.source_root.resolve()
    output = args.output.resolve()
    framework = args.framework_root.resolve()
    recompiler_build = args.recompiler_build.resolve()
    mkpsxiso = args.mkpsxiso.resolve()
    license_data = args.license_data.resolve()
    if output.exists():
        raise RuntimeError(f"output already exists; preserve it and choose a new path: {output}")

    source_exe = source_root / spec["exe"]
    source_expected = source_root / spec["expected"]
    require_hash(source_exe, spec["exe_sha256"])
    require_hash(source_expected, spec["expected_sha256"])
    for required in (
        recompiler_build / "psxrecomp-toml.exe",
        recompiler_build / "psxrecomp-game.exe",
        framework / "bios" / "OpenBIOS.toml",
        mkpsxiso,
        license_data,
    ):
        if not required.is_file():
            raise RuntimeError(f"missing required input: {required}")

    output.mkdir(parents=True)
    disc = output / "disc"
    disc.mkdir()
    shutil.copy2(source_exe, output / "test.exe")
    shutil.copy2(source_expected, output / "expected.log")
    shutil.copy2(license_data, disc / "license_data.dat")

    run_checked([
        str(recompiler_build / "psxrecomp-toml.exe"), "test.exe",
        "--output", "game.toml", "--seeds", "seeds.txt",
        "--name", args.fixture, "--id", "PSXT-00001",
    ], output)
    game_toml = output / "game.toml"
    text = game_toml.read_text(encoding="utf-8")
    bios_profile = (framework / "bios" / "OpenBIOS.toml").as_posix()
    text = text.replace(
        'seeds = ""',
        'seeds = "seeds.txt"',
    ).replace(
        'out_dir = "generated"',
        f'out_dir = "generated"\nbios_config = "{bios_profile}"',
    ).replace(
        '# debug_port = 4370',
        f'debug_port = {args.port}',
    )
    game_toml.write_text(text, encoding="utf-8", newline="\n")

    (disc / "SYSTEM.CNF").write_text(
        "BOOT = cdrom:\\TEST_000.01;1\nTCB = 4\nEVENT = 10\nSTACK = 801FFFF0\n",
        encoding="ascii", newline="\n")
    volume = "PSXTCPU" if args.fixture == "cpu-cop" else "PSXTGTE"
    (disc / "disc.xml").write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<iso_project image_name="test.bin" cue_sheet="test.cue" no_xa="0">
  <track type="data" xa_edc="true" new_type="true">
    <identifiers system="PLAYSTATION" application="PLAYSTATION"
      volume="{volume}" volume_set="{volume}" publisher="PSXRECOMP"
      data_preparer="MKPSXISO" copyright="SYSTEM.CNF;1"/>
    <license file="license_data.dat"/>
    <directory_tree>
      <file name="system.cnf" type="data" source="SYSTEM.CNF"/>
      <file name="test_000.01" type="data" source="../test.exe"/>
    </directory_tree>
  </track>
</iso_project>
''', encoding="utf-8", newline="\n")

    run_checked([str(mkpsxiso), "-y", "disc.xml"], disc)
    run_checked([str(recompiler_build / "psxrecomp-game.exe"), "--config", "game.toml"], output)

    receipt = {
        "schema_version": 1,
        "fixture": args.fixture,
        "class": spec["class"],
        "expected_assertions": spec["expected_assertions"],
        "source": catalog["source"],
        "framework": {"path": str(framework), "commit": git_head(framework)},
        "inputs": {
            "test.exe": sha256(output / "test.exe"),
            "expected.log": sha256(output / "expected.log"),
            "license_data.dat": sha256(disc / "license_data.dat"),
            "psxrecomp-toml.exe": sha256(recompiler_build / "psxrecomp-toml.exe"),
            "psxrecomp-game.exe": sha256(recompiler_build / "psxrecomp-game.exe"),
            "mkpsxiso.exe": sha256(mkpsxiso),
        },
        "outputs": {
            "disc/test.bin": sha256(disc / "test.bin"),
            "disc/test.cue": sha256(disc / "test.cue"),
            "generated/test.exe_dispatch.c": sha256(output / "generated" / "test.exe_dispatch.c"),
        },
        "oracle": {
            "upstream_expected_log": "available",
            "matched_hardware_recapture": "pending",
        },
        "debug_port": args.port,
    }
    (output / "prepare-receipt.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"prepared {args.fixture}: {output}")
    return 0


def tcp_query(port: int, payload: dict, timeout: float = 5.0) -> dict:
    wire = (json.dumps(payload, separators=(",", ":")) + "\n").encode("ascii")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(wire)
        stream = sock.makefile("rb")
        line = stream.readline()
    if not line:
        raise RuntimeError("debug server closed without a response")
    return json.loads(line)


def capture_tty(port: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    last = b""
    while time.monotonic() < deadline:
        try:
            response = tcp_query(port, {"cmd": "guest_tty_dump", "tail": 65536})
            if not response.get("ok"):
                raise RuntimeError(f"guest_tty_dump failed: {response}")
            last = bytes.fromhex(response.get("hex", ""))
            if "Done.\n" in normalized_log(last):
                return last
        except (ConnectionError, OSError):
            pass
        time.sleep(0.1)
    return last


def compare_capture(run_dir: Path, actual: bytes, result_path: Path) -> bool:
    expected_path = run_dir / "expected.log"
    expected = expected_path.read_bytes()
    expected_norm = comparable_log(normalized_log(expected))
    actual_raw_norm = normalized_log(actual)
    actual_norm = actual_raw_norm
    expected_lines = expected_norm.splitlines()
    if expected_lines:
        actual_lines = actual_raw_norm.splitlines()
        try:
            start = actual_lines.index(expected_lines[0])
        except ValueError:
            pass
        else:
            actual_norm = comparable_log("\n".join(actual_lines[start:]) + "\n")
    passed = actual_norm == expected_norm
    diff = "" if passed else "\n".join(difflib.unified_diff(
        expected_norm.splitlines(), actual_norm.splitlines(),
        fromfile="expected", tofile="actual", lineterm=""))
    result = {
        "schema_version": 1,
        "status": "pass" if passed else "fail",
        "expected_sha256": sha256(expected_path),
        "actual_sha256": hashlib.sha256(actual).hexdigest().upper(),
        "expected_normalized": expected_norm,
        "actual_raw_normalized": actual_raw_norm,
        "actual_normalized": actual_norm,
        "diff": diff,
    }
    result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    return passed


def run_fixture(args: argparse.Namespace) -> int:
    run_dir = args.run_dir.resolve()
    executable = args.executable.resolve()
    bios = args.bios.resolve()
    for path in (run_dir / "game.toml", run_dir / "disc" / "test.cue", executable, bios):
        if not path.is_file():
            raise RuntimeError(f"missing required run input: {path}")

    argv = [str(executable), "--no-launcher", "--headless",
            "--game", str(run_dir / "game.toml"),
            "--bios", str(bios), "--disc", str(run_dir / "disc" / "test.cue")]
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    proc = subprocess.Popen(argv, cwd=run_dir, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=creationflags)
    try:
        actual = capture_tty(args.port, args.timeout)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)

    result_path = run_dir / "result.json"
    passed = compare_capture(run_dir, actual, result_path)
    print(f"{run_dir.name}: {'PASS' if passed else 'FAIL'} ({len(actual)} guest-TTY bytes)")
    print(result_path)
    return 0 if passed else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    prep = sub.add_parser("prepare")
    prep.add_argument("--fixture", required=True, choices=sorted(load_catalog()["fixtures"]))
    prep.add_argument("--source-root", type=Path, required=True)
    prep.add_argument("--output", type=Path, required=True)
    prep.add_argument("--framework-root", type=Path, required=True)
    prep.add_argument("--recompiler-build", type=Path, required=True)
    prep.add_argument("--mkpsxiso", type=Path, required=True)
    prep.add_argument("--license-data", type=Path, required=True)
    prep.add_argument("--port", type=int, default=4601)
    prep.set_defaults(func=prepare)

    run = sub.add_parser("run")
    run.add_argument("--run-dir", type=Path, required=True)
    run.add_argument("--executable", type=Path, required=True)
    run.add_argument("--bios", type=Path, required=True)
    run.add_argument("--port", type=int, default=4601)
    run.add_argument("--timeout", type=float, default=30.0)
    run.set_defaults(func=run_fixture)
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        return args.func(args)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"conformance: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
