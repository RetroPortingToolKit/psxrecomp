#!/usr/bin/env bash
# Linux/macOS-shell launcher for tools/package_title_release.py (the one release
# command). Run from the title repository root:
#   psxrecomp/tools/package_title_release.sh --channel public|playtest
# Arguments pass through unchanged; --channel is required (no default).
set -euo pipefail
python="${PSX_RELEASE_PYTHON:-python3}"
command -v "$python" >/dev/null 2>&1 || {
  printf '\nPACKAGING FAILED: %s not found (Python 3.11+ required)\n' "$python" >&2; exit 1;
}
exec "$python" "$(cd "$(dirname "$0")" && pwd)/package_title_release.py" "$@"
