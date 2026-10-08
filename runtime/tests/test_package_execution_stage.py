"""Exercise the ZIP packager's real execution-staging gate without compression."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bash", default="bash")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    packager = (root / "tools/package_game_release.sh").read_text()
    marker = "# --- Bind the final executable to its original build execution contract ----"
    block = packager[packager.index(marker):packager.index('\nfind "${STAGE}" -exec touch')]
    assert packager.index(marker) > packager.index('bash "${SIGN_SH}"')
    assert packager.index(marker) < packager.index('"${SCRIPT_DIR}/create_release_zip.py"')
    shared = "a" * 64
    identity = hashlib.sha256(f"psx-execution-v1\nENHANCED\nshared:{shared}\n".encode()).hexdigest()
    manifest = dict(schema=1, profile="ENHANCED", identity=identity,
                    shared_contract_sha256=shared, implementations=[])
    checks = 0
    with tempfile.TemporaryDirectory(prefix="psx-package-execution-") as temp:
        work = Path(temp)
        # Periods and spaces in parent directories must not alter sidecar names.
        build, stage = work / "build.with space", work / "stage.with space"
        build.mkdir(); stage.mkdir()
        for name in ("Game.exe", "Game"):
            original, final = build / name, stage / name
            source = build / "Game.execution.json"
            output = stage / "Game.execution.json"
            source.write_text(json.dumps(manifest))
            original.write_bytes(identity.encode() + b"unsigned")
            final.write_bytes(identity.encode() + b"signed-final-bytes")
            # Staged stale metadata must never substitute for the build sidecar.
            output.write_text('{"identity":"stale staged metadata"}')
            script = 'set -euo pipefail\n' + '\n'.join(
                f"{key}='{value.as_posix() if isinstance(value, Path) else value}'"
                for key, value in dict(PY=Path(sys.executable), SCRIPT_DIR=root / "tools",
                                      STAGE=stage, EXE=original, EXE_BASENAME=name).items())
            script += '\n' + block + '\nprintf "after-stage\\n"\n'
            def run():
                return subprocess.run([args.bash, "-c", script], capture_output=True, text=True)
            result = run()
            assert result.returncode == 0 and "after-stage" in result.stdout, result.stderr
            bound = json.loads(output.read_text())
            assert bound["identity"] == identity
            assert bound["binary_sha256"] == hashlib.sha256(final.read_bytes()).hexdigest()
            checks += 1
            source.unlink()
            result = run()
            assert result.returncode != 0 and "after-stage" not in result.stdout
            checks += 1
            source.write_text(json.dumps(manifest))
            final.write_bytes(b"another executable lacking the contract identity")
            result = run()
            assert result.returncode != 0 and "after-stage" not in result.stdout
            checks += 1
        # Execute the actual stage-only removal/exit gates. An existing ZIP
        # must survive, and no compressor executable is needed or invoked.
        archive = work / "existing.zip"
        archive.write_bytes(b"preserved")
        removal = next(line for line in packager.splitlines() if '"${STAGE_ONLY}" != 1' in line)
        suffix = packager[packager.index('if [[ "${STAGE_ONLY}" == 1 ]]; then'):]
        script = f"set -euo pipefail\nSTAGE_ONLY=1\nDIST='{work.as_posix()}'\nZIP_NAME=existing.zip\nSTAGE='{stage.as_posix()}'\n" + removal + '\n' + suffix
        result = subprocess.run([args.bash,"-c",script],capture_output=True,text=True)
        assert result.returncode == 0 and "compression skipped" in result.stdout, result.stderr
        assert archive.read_bytes() == b"preserved"
        checks += 1
    print(f"PASS {checks} Windows/Linux-name packaging gates: final bytes, build sidecar, fail closed")


if __name__ == "__main__":
    main()
