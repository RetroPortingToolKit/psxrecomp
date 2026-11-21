#!/usr/bin/env bash
# regen_bios.sh — canonical, hygiene-safe regeneration of the BIOS generated C.
#
# WHY THIS EXISTS: generated/<stem>_*.c is gitignored build output. The
# recompiler (psxrecomp-bios) and the runtime are built by SEPARATE CMake trees,
# so editing the BIOS emitter (full_function_emitter.cpp, strict_translator.cpp,
# the cycle headers, …) does NOT automatically refresh generated/. If you forget
# to regenerate, the runtime links a STALE BIOS that no longer matches the emitter
# (this bit us: a stale 4439-entry BIOS vs the current 4406-entry output). This
# script makes regeneration one command AND records an emitter fingerprint so the
# CMake build can warn when generated/ has drifted (see runtime/runtime.cmake).
#
# PREREQUISITE: a CONFIGURED recompiler build dir. This script builds the
# emitter, it does not configure it — on a fresh clone run this first:
#   cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#
# Usage:
#   tools/regen_bios.sh                               # regen from bios/SCPH1001.toml
#   tools/regen_bios.sh --config bios/OpenBIOS.toml   # regen another BIOS profile
#   tools/regen_bios.sh --compare --config bios/OpenBIOS.toml
#       # rebuild, compare fresh C/h, refresh the stamp only if output is identical
#   PSXRECOMP_BIOS_BUILD=recompiler/build tools/regen_bios.sh   # custom build dir
#
# PSXRECOMP_BIOS_BUILD is relative to the framework ROOT (this script cd's there
# before using it), NOT to your shell's cwd. From a game project that vendors the
# framework, that is `recompiler/build` — never `psxrecomp/recompiler/build`.
#
# Legacy env-override form (bypasses the profile; used for scratch regens into a
# different out dir — fingerprint stem falls back to PSXRECOMP_BIOS_STEM/SCPH1001):
#   PSXRECOMP_BIOS_ROM=... PSXRECOMP_BIOS_SEEDS=... PSXRECOMP_BIOS_OUT=... tools/regen_bios.sh
#
# Run from the framework root (the dir containing recompiler/ and generated/).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PROFILE="bios/SCPH1001.toml"
COMPARE_ONLY=0
if [ "${1:-}" = "--compare" ]; then
    COMPARE_ONLY=1
    shift
fi
if [ $# -gt 0 ]; then
    if [ $# -eq 2 ] && [ "$1" = "--config" ] && [ -n "$2" ]; then
        PROFILE="$2"
    else
        # A typo'd flag silently regenerating the DEFAULT profile would be a
        # nasty surprise — reject anything but the exact supported form.
        echo "regen_bios: unrecognized arguments: $*" >&2
        echo "usage: tools/regen_bios.sh [--compare] [--config <bios profile.toml>]" >&2
        exit 2
    fi
fi

toml_value() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$PROFILE" | head -1
}

# Locate the recompiler build dir (where psxrecomp-bios[.exe] lives or will be
# built). Both failure paths below report the SAME missing step — configuring
# the recompiler — because that is what a fresh clone is actually missing. The
# old message here said "set PSXRECOMP_BIOS_BUILD", which sent people looking
# for an env var when no CMake tree existed to point it at; setting it to an
# unconfigured directory then skipped straight to the `cmake --build` below and
# died with cmake's bare "Error: could not load cache".
recompiler_configure_hint() {
    echo "regen_bios: configure the recompiler first (from $ROOT):" >&2
    echo "  cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
    echo "  cmake --build recompiler/build --target psxrecomp-bios" >&2
}

# A CMake configure that FAILS still leaves CMakeCache.txt behind — the cache is
# written before the generate step, which never runs after a FATAL_ERROR. So
# "has a cache" does not mean "is buildable": a directory left by a failed
# configure has a cache and no build.ninja, and `cmake --build` on it reports
# only `ninja: error: loading 'build.ninja': GetLastError() = 2`. Diagnose that
# here instead. The generator is read from the cache rather than guessed, and an
# unrecognized generator falls through to cmake rather than inventing a failure.
build_dir_unfinished_reason() {
    _bd=$1
    # CMake writes the cache LF-terminated even on Windows, but strip CR anyway
    # so a stray CRLF cannot make "Unix Makefiles\r" miss the *Makefiles glob.
    _gen=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$_bd/CMakeCache.txt" | tr -d '\r' | head -1)
    case "$_gen" in
        Ninja*)          [ -f "$_bd/build.ninja" ] || echo "no build.ninja (generator: $_gen)" ;;
        *"Makefiles")    [ -f "$_bd/Makefile" ]    || echo "no Makefile (generator: $_gen)" ;;
        "Visual Studio"*)
            ls "$_bd"/*.sln >/dev/null 2>&1 || echo "no .sln (generator: $_gen)" ;;
    esac
}
reject_unfinished_build_dir() {
    _reason=$(build_dir_unfinished_reason "$1")
    [ -n "$_reason" ] || return 0
    echo "regen_bios: $1 has a CMakeCache.txt but $_reason" >&2
    echo "regen_bios: that is what a FAILED configure leaves behind — the cache is written" >&2
    echo "regen_bios: before the generate step, so the build files were never produced." >&2
    echo "regen_bios: re-run the configure and let it finish; if it still fails, delete" >&2
    echo "regen_bios: $1 first so a stale cache cannot poison it." >&2
    recompiler_configure_hint
    exit 1
}

BUILD="${PSXRECOMP_BIOS_BUILD:-}"
if [ -n "$BUILD" ]; then
    # An explicit setting bypasses discovery, so validate it here — discovery
    # requires a CMakeCache.txt and an override should not be held to less.
    if [ ! -f "$BUILD/CMakeCache.txt" ]; then
        echo "regen_bios: PSXRECOMP_BIOS_BUILD=$BUILD is not a configured CMake build dir" >&2
        echo "regen_bios: (no $BUILD/CMakeCache.txt — the path is relative to the framework" >&2
        echo "regen_bios:  root $ROOT, not your shell's cwd)" >&2
        recompiler_configure_hint
        exit 1
    fi
else
    # ../build-recompiler: where a game repo's tools/ci/build_emitters.sh
    # builds when the framework is its psxrecomp/ submodule.
    for d in recompiler/build-t2 recompiler/build recompiler/cmake-build* ../build-recompiler; do
        [ -f "$d/CMakeCache.txt" ] || continue
        # Skip a directory left by a failed configure and keep looking: picking
        # it and dying is strictly worse than using the next candidate that
        # actually generated.
        [ -z "$(build_dir_unfinished_reason "$d")" ] || continue
        BUILD="$d"; break
    done
fi
[ -n "$BUILD" ] || {
    echo "regen_bios: no usable recompiler build dir found under $ROOT" >&2
    echo "regen_bios: (looked for a CMakeCache.txt plus finished build files in" >&2
    echo "regen_bios:  recompiler/build-t2, recompiler/build, recompiler/cmake-build*;" >&2
    echo "regen_bios:  a dir left by a failed configure is skipped. Set" >&2
    echo "regen_bios:  PSXRECOMP_BIOS_BUILD to use a different one.)" >&2
    recompiler_configure_hint
    exit 1
}
reject_unfinished_build_dir "$BUILD"

if [ "$COMPARE_ONLY" -eq 1 ]; then
    if [ -n "${PSXRECOMP_BIOS_ROM:-}${PSXRECOMP_BIOS_SEEDS:-}${PSXRECOMP_BIOS_OUT:-}" ]; then
        echo "regen_bios: unverified: --compare requires a pinned profile, not legacy env overrides" >&2
        exit 4
    fi
    [ -f "$PROFILE" ] || { echo "regen_bios: unverified: profile missing: $PROFILE" >&2; exit 4; }
    _compare_pin="$(toml_value sha256)"
    if [[ ! "$_compare_pin" =~ ^[[:xdigit:]]{64}$ ]]; then
        echo "regen_bios: unverified: --compare requires the profile's quoted image sha256 pin" >&2
        exit 4
    fi
    # A configured tree for another checkout can build successfully but cannot
    # certify this checkout's fingerprint. Resolve native Windows paths too.
    _compare_source_dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt" | tr -d '\r' | head -1)
    if command -v cygpath >/dev/null 2>&1; then
        _compare_source_dir=$(cygpath -u "$_compare_source_dir")
    fi
    if [ ! -d "$_compare_source_dir" ] ||
       [ "$(cd "$_compare_source_dir" && pwd -P)" != "$(cd "$ROOT/recompiler" && pwd -P)" ]; then
        echo "regen_bios: unverified: $BUILD is configured for another recompiler source tree" >&2
        exit 4
    fi
    _compare_before=$("$BASH" "$ROOT/tools/bios_emitter_fingerprint.sh" "$PROFILE")
fi

# 1. Build the emitter FRESH so the regen always reflects current source.
echo "regen_bios: building psxrecomp-bios in $BUILD"
cmake --build "$BUILD" --target psxrecomp-bios >/dev/null

EXE="$BUILD/psxrecomp-bios.exe"; [ -f "$EXE" ] || EXE="$BUILD/psxrecomp-bios"
[ -f "$EXE" ] || { echo "regen_bios: psxrecomp-bios not found in $BUILD after build"; exit 1; }

if [ -n "${PSXRECOMP_BIOS_ROM:-}${PSXRECOMP_BIOS_SEEDS:-}${PSXRECOMP_BIOS_OUT:-}" ]; then
    # 2a. Legacy env-override regen (positional invocation, profile bypassed).
    BIOS="${PSXRECOMP_BIOS_ROM:-bios/SCPH1001.BIN}"
    SEEDS="${PSXRECOMP_BIOS_SEEDS:-recompiler/seeds/phase2_ghidra_seeds.json}"
    OUT="${PSXRECOMP_BIOS_OUT:-generated}"
    STEM="${PSXRECOMP_BIOS_STEM:-SCPH1001}"
    echo "regen_bios: emit-full $BIOS -> $OUT (seeds: $SEEDS) [env-override mode]"
    mkdir -p "$OUT"
    "$EXE" "$BIOS" "$OUT" --emit-full "$SEEDS"
else
    # 2b. Profile-driven regen (the canonical path).
    [ -f "$PROFILE" ] || { echo "regen_bios: profile not found: $PROFILE"; exit 1; }
    OUT="$(toml_value out_dir)";  OUT="${OUT:-generated}"
    STEM="$(toml_value out_stem)"
    [ -n "$STEM" ] || { echo "regen_bios: no out_stem in $PROFILE"; exit 1; }
    if [ "$COMPARE_ONLY" -eq 1 ] && [[ ! "$STEM" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]]; then
        echo "regen_bios: unverified: --compare requires a flat out_stem, without path separators" >&2
        exit 4
    fi
    echo "regen_bios: emit-full --config $PROFILE (stem: $STEM)"
    mkdir -p "$OUT"
    if [ "$COMPARE_ONLY" -eq 1 ]; then
        # Compare a genuinely fresh output set: emitting over generated/ would
        # conceal files the generator stopped producing. Include underscore
        # and dot suffixes so an added/removed C file or header is not ignored.
        shopt -s nullglob
        _compare_old=("$OUT/${STEM}_"*.c "$OUT/${STEM}_"*.h "$OUT/${STEM}."*.c "$OUT/${STEM}."*.h)
        if [ ${#_compare_old[@]} -eq 0 ]; then
            echo "regen_bios: unverified: no previous $STEM C/h output; run normal regeneration first" >&2
            exit 4
        fi
        _compare_tmp_root=$(cd "${TMPDIR:-/tmp}" && pwd -P)
        # A PATH utility may come from another MSYS installation, whose /tmp
        # names a different directory. Use this shell's own converter so the
        # native emitter and every comparison utility see the same files.
        if [ -x "${BASH%/*}/cygpath" ]; then
            _compare_tmp_root=$("${BASH%/*}/cygpath" -m "$_compare_tmp_root")
        fi
        _compare_dir=$(mktemp -d "$_compare_tmp_root/psx-bios-compare.XXXXXXXX")
        _compare_stamp_tmp=""
        cleanup_compare() {
            # Only this fresh directory is owned. Never recurse through an
            # unexpected directory/link or remove protected/unexpected trees.
            if [[ "$_compare_dir" != "$_compare_tmp_root"/psx-bios-compare.* ]] ||
               [ -L "$_compare_dir" ]; then
                echo "regen_bios: retained unexpected comparison path $_compare_dir" >&2
                return
            fi
            _compare_entries=("$_compare_dir"/* "$_compare_dir"/.[!.]* "$_compare_dir"/..?*)
            for _entry in "${_compare_entries[@]}"; do
                if [ ! -f "$_entry" ] || [ -L "$_entry" ]; then
                    echo "regen_bios: retained comparison directory with unexpected entry: $_compare_dir" >&2
                    return
                fi
            done
            if [ ${#_compare_entries[@]} -gt 0 ]; then rm -- "${_compare_entries[@]}"; fi
            rmdir -- "$_compare_dir"
            if [ -n "$_compare_stamp_tmp" ] && [ -f "$_compare_stamp_tmp" ]; then
                rm -- "$_compare_stamp_tmp"
            fi
        }
        trap cleanup_compare EXIT
        if ! "$EXE" --config "$PROFILE" --out-dir "$_compare_dir"; then
            echo "regen_bios: $STEM unverified: regeneration failed; previous output/stamp unchanged" >&2
            exit 4
        fi
        _compare_after=$("$BASH" "$ROOT/tools/bios_emitter_fingerprint.sh" "$PROFILE")
        if [ "$_compare_before" != "$_compare_after" ]; then
            echo "regen_bios: $STEM unverified: inputs changed during build/regeneration; stamp unchanged" >&2
            exit 4
        fi
        _compare_new=("$_compare_dir/${STEM}_"*.c "$_compare_dir/${STEM}_"*.h "$_compare_dir/${STEM}."*.c "$_compare_dir/${STEM}."*.h)
        if [ ${#_compare_new[@]} -eq 0 ]; then
            echo "regen_bios: $STEM unverified: no fresh C/h output; stamp unchanged" >&2
            exit 4
        fi
        _compare_changed=0
        for _file in "${_compare_new[@]}"; do
            _name=${_file##*/}
            _compare_result=0
            if [ -L "$_file" ] || [ ! -f "$OUT/$_name" ] || [ -L "$OUT/$_name" ]; then
                _compare_result=1
            else
                cmp -s "$_file" "$OUT/$_name" || _compare_result=$?
                if [ "$_compare_result" -gt 1 ]; then
                    echo "regen_bios: $STEM unverified: cannot compare $_name; previous output/stamp unchanged" >&2
                    exit 4
                fi
            fi
            if [ "$_compare_result" -eq 1 ]; then
                echo "regen_bios: $STEM generated output changed: $_name" >&2
                _compare_changed=1
            fi
        done
        for _file in "${_compare_old[@]}"; do
            if [ ! -f "$_compare_dir/${_file##*/}" ]; then
                echo "regen_bios: $STEM generated output removed: ${_file##*/}" >&2
                _compare_changed=1
            fi
        done
        if [ "$_compare_changed" -ne 0 ]; then
            echo "regen_bios: $STEM output changed; previous output/stamp unchanged. Run normal regeneration and review the change." >&2
            exit 3
        fi
        _compare_stamp_tmp=$(mktemp "$OUT/${STEM}.emitter.sha.XXXXXXXX")
        printf '%s\n' "$_compare_after" > "$_compare_stamp_tmp"
        mv -- "$_compare_stamp_tmp" "$OUT/$STEM.emitter.sha"
        _compare_stamp_tmp=""
        echo "regen_bios: $STEM output-neutral (${#_compare_new[@]} C/h files); verified fingerprint refreshed"
        exit 0
    fi
    "$EXE" --config "$PROFILE"
fi

# 3. Record the emitter fingerprint so the build can detect future drift. MUST
#    match runtime/runtime.cmake's staleness check invocation (same profile arg).
"$BASH" "$ROOT/tools/bios_emitter_fingerprint.sh" "$PROFILE" > "$OUT/$STEM.emitter.sha"
echo "regen_bios: wrote fingerprint $OUT/$STEM.emitter.sha"
echo "regen_bios: done ($(grep -m1 -o 'Dispatch entries: [0-9]*' "$OUT/${STEM}_dispatch.c" || echo '?'))"
