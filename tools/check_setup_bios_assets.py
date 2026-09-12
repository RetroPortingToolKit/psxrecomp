#!/usr/bin/env python3
"""Require BIOS assets selected by the staged title recipe."""
import sys
from pathlib import Path
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib


def check(stage):
    stage = Path(stage).resolve()
    recipe = stage / "game.toml"
    if not recipe.is_file():
        raise ValueError("missing staged game.toml for BIOS policy")
    with recipe.open("rb") as handle:
        config = tomllib.load(handle)
    openbios = config.get("runtime", {}).get("openbios", True)
    if not isinstance(openbios, bool):
        raise ValueError("runtime.openbios must be a boolean")
    profile = config.get("recompiler", {}).get("bios_config")
    required = []
    if profile:
        required.append(stage / profile)
    elif not openbios:
        # Existing title recipes without a profile use the CLI default.
        required.append(stage / "psxrecomp/bios/SCPH1001.toml")
    if openbios:
        required.extend(stage / "psxrecomp/bios" / name for name in
                        ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"))
    for path in required:
        path = path.resolve()
        try:
            path.relative_to(stage)
        except ValueError:
            raise ValueError("BIOS asset must remain inside the staged kit")
        if not path.is_file():
            raise ValueError("missing staged BIOS asset: " + str(path.relative_to(stage)))
    return [str(path.relative_to(stage)) for path in required]


if __name__ == "__main__":
    try:
        print("staged BIOS policy: " + ", ".join(check(sys.argv[1])))
    except (ValueError, OSError) as error:
        sys.exit("error: " + str(error))
