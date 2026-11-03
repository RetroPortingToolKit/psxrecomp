#!/usr/bin/env bash
# Official optimized Windows/Linux release command. Requires the title disc.
# Build, private boot training, staging and verification belong to the shared
# builder; --channel public|playtest is required and has no default.
# Usage: scripts/package_release.sh --channel public [--platform windows|linux|all]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PACKAGER="${ROOT}/psxrecomp/tools/package_title_release.sh"
if [[ ! -f "${PACKAGER}" ]]; then
  echo "error: missing ${PACKAGER} (update the psxrecomp submodule)" >&2
  exit 1
fi
exec bash "${PACKAGER}" --root "${ROOT}" --config packaging/release.toml "$@"
