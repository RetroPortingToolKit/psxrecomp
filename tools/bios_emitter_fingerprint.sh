#!/usr/bin/env bash
# bios_emitter_fingerprint.sh — thin wrapper: the fingerprint is computed by
# tools/bios_emitter_fingerprint.cmake (one implementation, run by cmake -P on
# every platform; see its header for why a bash implementation could not be
# trusted across runners). regen_bios.sh writes the result into
# generated/<stem>.emitter.sha; runtime.cmake recomputes it at configure.
#
# Usage: bios_emitter_fingerprint.sh [--list] [profile.toml]
#   (default profile: bios/SCPH1001.toml)
#   --list prints the hashed inputs, "<kind>\t<present|absent>\t<path>", instead.
#   PSXRECOMP_FP_DEBUG=1 also prints "<digest>  <path>" per input to stderr.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="bios/SCPH1001.toml"
args=(-DROOT="$ROOT")
for arg in "$@"; do
  case "$arg" in
    --list) args+=(-DFP_LIST=1) ;;
    --*) echo "bios_emitter_fingerprint: unknown option $arg" >&2; exit 2 ;;
    *) PROFILE="$arg" ;;
  esac
done
CMAKE_BIN="${CMAKE:-cmake}"
command -v "$CMAKE_BIN" >/dev/null 2>&1 || { echo "bios_emitter_fingerprint: cmake not found on PATH" >&2; exit 1; }
args+=(-DPROFILE="$PROFILE")
[ -n "${RECOMP_SHARED_CORE_ROOT:-}" ] && args+=(-DRECOMP_SHARED_CORE_ROOT="$RECOMP_SHARED_CORE_ROOT")
[ -n "${PSXRECOMP_FP_DEBUG:-}" ] && args+=(-DFP_DEBUG=1)
exec "$CMAKE_BIN" "${args[@]}" -P "$ROOT/tools/bios_emitter_fingerprint.cmake"
