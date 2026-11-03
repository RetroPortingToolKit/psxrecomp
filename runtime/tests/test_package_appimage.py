"""Exercise AppDir routing with tiny verified stand-in tools, never a game/build.

The production provenance pins are asserted; only a PRIVATE script copy uses
fixture-tool hashes/file URLs. This checks final-byte identity binding after a
deployment rewrite, mandatory cache verification, and compression failure gates.
It does not certify a real ELF executable, linuxdeploy, or an AppImage artifact.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "tools/package_appimage.sh").read_text()
    deploy_pin = "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
    image_pin = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
    assert deploy_pin in source and image_pin in source
    assert source.index('"$LINUXDEPLOY" --appimage') < source.index('"$PY" "$HELPER" stage-execution') < source.index('ARCH=x86_64 "$APPIMAGETOOL"')
    assert source.index('"$PY" "$HELPER" fetch-pinned') < source.index('"$LINUXDEPLOY" --appimage')
    shared = "a" * 64
    identity = hashlib.sha256(f"psx-execution-v1\nENHANCED\nshared:{shared}\n".encode()).hexdigest()
    manifest = dict(schema=1, profile="ENHANCED", identity=identity,
                    shared_contract_sha256=shared, implementations=[], binary_sha256="f" * 64)
    with tempfile.TemporaryDirectory(prefix="psx-appimage-fixture-") as temp:
        work = Path(temp)
        payload, cache, remote, tmp = [work / name for name in ("payload with.space", "cache", "remote", "tmp")]
        for directory in (payload,cache,remote,tmp): directory.mkdir()
        (payload / "assets").mkdir()
        original = b"\x7fELF" + identity.encode() + b"original payload bytes"
        (payload / "Game").write_bytes(original)
        (payload / "Game.execution.json").write_text(json.dumps(manifest))
        (payload / "game.toml").write_text("[game]\nname='Fixture'\n")
        app_run, desktop, icon = [work / name for name in ("AppRun", "io.fixture.Game.desktop", "icon.png")]
        app_run.write_text("#!/bin/sh\nexit 0\n")
        desktop.write_text("[Desktop Entry]\nType=Application\nName=Fixture\nExec=Game\nIcon=io.fixture.Game\nCategories=Game;\n")
        icon.write_bytes(b"\x89PNG\r\n\x1a\nfixture")
        tools = {
            "linuxdeploy-x86_64.AppImage": '#!/bin/bash\nset -eu\nwhile [ "$#" -gt 0 ]; do if [ "$1" = --executable ]; then exe=$2; shift 2; else shift; fi; done\nprintf "deployment rewrite" >> "$exe"\n',
            "appimagetool-x86_64.AppImage": '#!/bin/bash\nset -eu\nout="${@: -1}"\nprintf "fixture squashfs output" > "$out"\n',
        }
        for name,text in tools.items():
            (remote / name).write_text(text)
            # Invalid cached bytes must be replaced through verified fetching.
            (cache / name).write_text("invalid previous cache")
        deploy_url = "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
        image_url = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
        private = source.replace(deploy_pin,hashlib.sha256((remote / "linuxdeploy-x86_64.AppImage").read_bytes()).hexdigest()).replace(image_pin,hashlib.sha256((remote / "appimagetool-x86_64.AppImage").read_bytes()).hexdigest())
        private = private.replace(deploy_url,(remote / "linuxdeploy-x86_64.AppImage").as_uri()).replace(image_url,(remote / "appimagetool-x86_64.AppImage").as_uri())
        script = work / "package_appimage.sh"
        script.write_text(private)
        environment = dict(os.environ,TMPDIR=str(tmp),SOURCE_DATE_EPOCH="12345")
        def run(output):
            return subprocess.run(["bash",str(script),"--framework",str(root),"--payload",str(payload),"--exe-name","Game","--payload-name","fixture","--app-run",str(app_run),"--desktop-file",str(desktop),"--icon",str(icon),"--tool-cache",str(cache),"--output",str(output),"--jobs","2"],env=environment,text=True,capture_output=True)
        output = work / "candidate.AppImage"
        result = run(output)
        assert result.returncode == 0, result.stdout + result.stderr
        appdir = Path(next(line.split("AppDir: ",1)[1] for line in result.stdout.splitlines() if line.startswith("AppDir: ")))
        final = (appdir / "usr/bin/Game").read_bytes()
        bound = json.loads((appdir / "usr/bin/Game.execution.json").read_text())
        assert final == original + b"deployment rewrite"
        assert bound["binary_sha256"] == hashlib.sha256(final).hexdigest()
        assert bound["binary_sha256"] != manifest["binary_sha256"]
        assert (appdir / "usr/bin/assets").is_symlink()
        assert not (appdir / "usr/share/fixture/Game").exists()
        assert output.is_file() and Path(str(output)+".sha256").is_file()
        for name in tools: assert (cache / name).read_bytes() == (remote / name).read_bytes()
        # A bad build contract must fail before the stand-in compressor runs.
        manifest["identity"] = "0" * 64
        (payload / "Game.execution.json").write_text(json.dumps(manifest))
        failed = work / "failed.AppImage"
        result = run(failed)
        assert result.returncode != 0 and not failed.exists()
        assert "execution staging failed" in result.stderr
        (payload / "wrong.dll").write_bytes(b"Windows cache")
        result = run(failed)
        assert result.returncode != 0 and "Windows binaries/cache" in result.stderr
    print("PASS AppImage routing: verified tools, post-deploy final bytes, no stale hash, fail closed")


if __name__ == "__main__":
    main()
