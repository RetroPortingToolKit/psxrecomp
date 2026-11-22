"""Every committed generated/<stem>.emitter.sha matches the current emitter.

runtime.cmake compares these stamps only at title configure time, and only
fatally in release CI, so a change to a fingerprint input (emitter sources,
cycle headers, a profile or its seeds) that forgets to restamp sails through
the test suite. This test makes that drift fail here instead.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", required=True)
    ap.add_argument("--root", type=Path,
                    default=Path(__file__).resolve().parents[2])
    args = ap.parse_args()
    root = args.root
    stamps = sorted((root / "generated").glob("*.emitter.sha"))
    assert stamps, "no generated/*.emitter.sha stamps found"
    stale = []
    for stamp in stamps:
        stem = stamp.name[:-len(".emitter.sha")]
        profile = root / "bios" / (stem + ".toml")
        assert profile.exists(), f"{stamp.name} has no bios/{stem}.toml profile"
        p = subprocess.run(
            [args.cmake, f"-DROOT={root}", f"-DPROFILE=bios/{stem}.toml",
             "-P", str(root / "tools" / "bios_emitter_fingerprint.cmake")],
            cwd=root, capture_output=True, text=True)
        assert p.returncode == 0, p.stderr
        now = p.stdout.strip().splitlines()[-1].strip()
        saved = stamp.read_text(encoding="utf-8").strip()
        if now != saved:
            stale.append(f"{stem}: stamp {saved} now {now}")
    assert not stale, ("stale BIOS emitter stamps (for output-neutral changes run "
                       "tools/regen_bios.sh --compare --config bios/<stem>.toml; "
                       "commit the verified OpenBIOS stamp with the source change):\n  "
                       + "\n  ".join(stale))
    print(f"PASS: {len(stamps)} BIOS emitter stamps match the emitter")


if __name__ == "__main__":
    main()
