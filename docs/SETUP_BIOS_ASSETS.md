# Setup SDK BIOS assets

The staged game.toml selects the required BIOS profile. Retail-only recipes do not require the unused OpenBIOS image and license.
Default OpenBIOS recipes still require their profile, image and license. The selected profile must be a file inside the staged kit.
The gate does not bundle retail BIOS dumps and does not change the CLI's BIOS selection or generated-code readiness checks.

Every required asset is resolved before the containment check, including default profiles and OpenBIOS assets. A file or parent-directory symlink cannot make an external asset count as part of the kit. Symlinks whose targets remain inside the kit are allowed.

Run `python runtime/tests/test_setup_bios_assets.py` for ten synthetic fixture tests. Symlink controls run when the host permits symlink creation; other controls always run.
The existing `stage_setup_sdk.sh` gate calls the same production checker. Python 3.11 uses tomllib; older Python needs tomli, as other setup tools do.

This gate is independently useful for retail SCPH1001 setup. Full selected-profile/SCPH5552 support also needs matching CLI, profile and generation support.
The current upstream CLI still hardcodes SCPH1001 in its retail generate path. Passing this gate alone does not establish SCPH5552 setup support.
