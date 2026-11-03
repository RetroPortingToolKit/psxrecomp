#!/usr/bin/env bash
# Compare every committed BIOS backend's emitter stamp with the fingerprint
# computed on THIS machine, and on mismatch print the per-input digests so the
# disagreement is attributable (which file, not just "stale"). runtime.cmake
# performs the same comparison at configure (PSXRECOMP_BIOS_STALE_FATAL in
# CI); this runs first so the failure is readable.
#
# Usage: check_bios_stamps.sh [--framework psxrecomp] [stem...]
# Default: every stem that has a generated/<stem>.emitter.sha. Retail backends
# (SCPH1001, ...) are gitignored -- generated from a dump, never committed -- so
# a CI checkout carries only the OpenBIOS stamp and must not demand the others.
# Name a stem explicitly to require it.
set -euo pipefail
FRAMEWORK="psxrecomp"
if [[ "${1:-}" == "--framework" ]]; then FRAMEWORK="${2:?}"; shift 2; fi
FRAMEWORK="$(cd "${FRAMEWORK}" && pwd)"
STEMS=("$@")
if [[ ${#STEMS[@]} -eq 0 ]]; then
  for f in "${FRAMEWORK}"/generated/*.emitter.sha; do
    [[ -f "${f}" ]] || continue
    b="$(basename "${f}")"; STEMS+=("${b%.emitter.sha}")
  done
  [[ ${#STEMS[@]} -gt 0 ]] || { echo "error: no generated/*.emitter.sha in ${FRAMEWORK}" >&2; exit 1; }
fi
rc=0
for stem in "${STEMS[@]}"; do
  stamp_file="${FRAMEWORK}/generated/${stem}.emitter.sha"
  profile="bios/${stem}.toml"
  if [[ ! -f "${stamp_file}" ]]; then echo "error: ${stamp_file} missing" >&2; rc=1; continue; fi
  stamp="$(tr -d '[:space:]' < "${stamp_file}")"
  now="$(cd "${FRAMEWORK}" && bash tools/bios_emitter_fingerprint.sh "${profile}")"
  if [[ "${stamp}" == "${now}" ]]; then
    echo "${stem}: stamp current (${now:0:12})"
  else
    echo "error: ${stem}: stamp ${stamp:0:12} != fingerprint ${now:0:12} on this machine" >&2
    echo "  per-input digests on this machine:" >&2
    (cd "${FRAMEWORK}" && PSXRECOMP_FP_DEBUG=1 bash tools/bios_emitter_fingerprint.sh "${profile}" 2>&1 >/dev/null | sed 's/^/    /') >&2
    echo "  tools: $(command -v bash) $(bash --version | head -1); $(command -v sha256sum); $(command -v tr)" >&2
    echo "  core.autocrlf=$(git -C "${FRAMEWORK}" config --get core.autocrlf || echo unset)" >&2
    rc=1
  fi
done
exit "${rc}"
