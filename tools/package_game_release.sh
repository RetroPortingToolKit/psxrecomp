#!/usr/bin/env bash
# Bundled-release zip packager for PSXRecomp game repos: ships the COMPILED
# game, not a kit that compiles it.
#
# A release built from the title's committed generated/ C is a finished
# product. The zip carries the game executable and everything the runtime
# reads beside it -- assets, the mod catalog, the bundled OpenBIOS image and
# its notice, game.toml / game_options.toml, third-party notices -- plus the
# overlay toolchain so overlays outside the compiled static shard can still be
# turned native on the player's machine. It carries NO sources, NO emitters at
# the root, NO psxrecomp/ or recomp-ui/ tree, NO CLI, NO generated C and NO
# BIOS dump. The player supplies a legal disc and plays.
#
# Usage (from game repo root):
#   psxrecomp/tools/package_game_release.sh \
#     --build-dir build-ci \
#     --artifact linux-x64 \
#     --zip-prefix bpe \
#     --exe-name Bomberman_Party_Edition_Recompiled \
#     --display-name "Bomberman Party Edition Recompiled" \
#     --recompiler-build build-recompiler \
#     [--runtime-file REL]...            # extra exe-relative files (keybinds.ini, ...)
#     [--runtime-dir NAME]...            # extra exe-relative dirs (bezels, ...)
#     [--doc REL]...                     # extra docs from the repo (DISC.md, ...)
#     [--overlay-cache-root DIR]         # ship a prebuilt overlay shard cache
#     [--ship-without-overlay-cache-because REASON]
#     [--no-overlay-toolchain]           # do not bundle overlay_toolchain/
#     [--disc-hint "your legally owned disc"] [--version-env RELEASE_VERSION]
#
# Overlay cache. The runtime's overlay shard cache is compiled FROM THE DISC,
# so a CI job -- which has no disc -- cannot build one; the committed static
# AOT shard (generated/overlays_static*.c) is compiled into the executable
# instead, and the bundled overlay_toolchain/ compiles the rest from the
# player's own disc at runtime. A developer packaging locally with a cache
# passes --overlay-cache-root and it is staged through release_stage.py
# (which owns the cache tag and layout; nothing here knows either). Without a
# cache the caller must say why, in words, and the reason is printed: this is
# the same deliberately unwieldy switch release_stage.py exposes, because a
# packager that quietly staged nothing once looked like a good release twice.
#
# DEVELOPER CHANNEL. mods with channel = "developer" are pruned from public
# releases (EXCLUDE_DEV_MODS=1, which CI sets via $CI) and kept in local
# exports; --include-dev-mods / --exclude-dev-mods override. Pruning is by
# package directory via tools/mod_channel_filter.py, never by editing a
# manifest.
#
# Env:
#   RELEASE_VERSION / <version-env> / VERSION file  (must match binary stamp)
#   PSXRECOMP_RUNTIME_BIN_DIR | BPE_RUNTIME_BIN_DIR  (Windows MinGW DLL search)
#   PSX_RELEASE_STAGE_PYTHON                         (python3 override)
#
# Lobby pin: the exe must have been built with current runtime.cmake so
# $<TARGET_FILE_DIR>/psx_game_version.txt exists. This script refuses to ship
# a VERSION that disagrees with that stamp (netplay list filter bug).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FRAMEWORK="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT="$(pwd)"

BUILD_DIR=""
ARTIFACT=""
ZIP_PREFIX=""
EXE_NAME=""
DISPLAY_NAME=""
RECOMPILER_BUILD="build-recompiler"
VERSION_ENV="RELEASE_VERSION"
DISC_HINT="your legally owned game disc"
RUNTIME_TARGET="psx-runtime"
RUNTIME_FILES=()
RUNTIME_DIRS=()
DOCS=()
OVERLAY_CACHE_ROOT=""
SHIP_WITHOUT_OVERLAY_CACHE_BECAUSE=""
SHIP_WITHOUT_OVERLAY_CACHE_KEY_BECAUSE=""
STAGE_OVERLAY_TOOLCHAIN=1
if [[ -z "${EXCLUDE_DEV_MODS:-}" ]]; then
  if [[ -n "${CI:-}" ]]; then EXCLUDE_DEV_MODS=1; else EXCLUDE_DEV_MODS=0; fi
fi
RUNTIME_BIN_DIR="${PSXRECOMP_RUNTIME_BIN_DIR:-${BPE_RUNTIME_BIN_DIR:-/usr/x86_64-w64-mingw32/bin}}"

usage() {
  sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-2}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --artifact) ARTIFACT="${2:?}"; shift 2 ;;
    --zip-prefix) ZIP_PREFIX="${2:?}"; shift 2 ;;
    --exe-name) EXE_NAME="${2:?}"; shift 2 ;;
    --display-name) DISPLAY_NAME="${2:?}"; shift 2 ;;
    --recompiler-build) RECOMPILER_BUILD="${2:?}"; shift 2 ;;
    --runtime-target) RUNTIME_TARGET="${2:?}"; shift 2 ;;
    --version-env) VERSION_ENV="${2:?}"; shift 2 ;;
    --disc-hint) DISC_HINT="${2:?}"; shift 2 ;;
    --runtime-file) RUNTIME_FILES+=("${2:?}"); shift 2 ;;
    --runtime-dir) RUNTIME_DIRS+=("${2:?}"); shift 2 ;;
    --doc) DOCS+=("${2:?}"); shift 2 ;;
    --overlay-cache-root) OVERLAY_CACHE_ROOT="${2:?}"; shift 2 ;;
    --ship-without-overlay-cache-because) SHIP_WITHOUT_OVERLAY_CACHE_BECAUSE="${2:?}"; shift 2 ;;
    --ship-without-overlay-cache-key-because) SHIP_WITHOUT_OVERLAY_CACHE_KEY_BECAUSE="${2:?}"; shift 2 ;;
    --no-overlay-toolchain) STAGE_OVERLAY_TOOLCHAIN=0; shift ;;
    --exclude-dev-mods) EXCLUDE_DEV_MODS=1; shift ;;
    --include-dev-mods) EXCLUDE_DEV_MODS=0; shift ;;
    --runtime-bin) RUNTIME_BIN_DIR="${2:?}"; shift 2 ;;
    --root) ROOT="${2:?}"; shift 2 ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage 2
      ;;
  esac
done

if [[ -z "${BUILD_DIR}" || -z "${ARTIFACT}" || -z "${ZIP_PREFIX}" || -z "${EXE_NAME}" ]]; then
  echo "error: --build-dir, --artifact, --zip-prefix, and --exe-name are required" >&2
  usage 2
fi
[[ -n "${DISPLAY_NAME}" ]] || DISPLAY_NAME="${EXE_NAME}"

ROOT="$(cd "${ROOT}" && pwd)"
if [[ -d "${ROOT}/${BUILD_DIR}" ]]; then
  BUILD_DIR="$(cd "${ROOT}/${BUILD_DIR}" && pwd)"
elif [[ -d "${BUILD_DIR}" ]]; then
  BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
else
  echo "error: build dir not found: ${BUILD_DIR}" >&2
  exit 1
fi
if [[ -d "${ROOT}/${RECOMPILER_BUILD}" ]]; then
  RECOMPILER_BUILD="$(cd "${ROOT}/${RECOMPILER_BUILD}" && pwd)"
elif [[ -d "${RECOMPILER_BUILD}" ]]; then
  RECOMPILER_BUILD="$(cd "${RECOMPILER_BUILD}" && pwd)"
fi

# The shared staging surface: cache tag / shards / toolchain / mod catalog are
# release_stage.py's, and only its. This script marshals arguments.
# shellcheck source=release_overlay_stage.sh
. "${SCRIPT_DIR}/release_overlay_stage.sh"
psx_release_stage_init "${FRAMEWORK}"
PY="${_psx_py}"

# --- Version: the compile-time stamp is authoritative ------------------------
REQUESTED=""
if [[ -n "${VERSION_ENV}" ]]; then REQUESTED="${!VERSION_ENV:-}"; fi
if [[ -z "${REQUESTED}" && -n "${RELEASE_VERSION:-}" ]]; then REQUESTED="${RELEASE_VERSION}"; fi
if [[ -z "${REQUESTED}" && -f "${ROOT}/VERSION" ]]; then REQUESTED="$(tr -d '[:space:]' <"${ROOT}/VERSION")"; fi
REQUESTED="$(printf '%s' "${REQUESTED}" | tr -d '[:space:]')"
REQUESTED="${REQUESTED#v}"

EXE=""
# Git Bash's -f accepts a Windows PE through its unsuffixed alias. Prefer the
# literal .exe path so extension-based DLL bundling and signing cannot be skipped.
for cand in "${BUILD_DIR}/${EXE_NAME}.exe" "${BUILD_DIR}/Release/${EXE_NAME}.exe" "${BUILD_DIR}/${EXE_NAME}"; do
  if [[ -f "${cand}" ]]; then EXE="${cand}"; break; fi
done
if [[ -z "${EXE}" ]]; then
  # CMake records the OUTPUT_NAME it chose; prefer it over the wrapper's copy
  # so a renamed title does not read as "binary missing".
  marker="${BUILD_DIR}/psxrecomp_exe_name-${RUNTIME_TARGET}.txt"
  if [[ -f "${marker}" ]]; then
    chosen="$(tr -d '[:space:]' <"${marker}")"
    for cand in "${BUILD_DIR}/${chosen}.exe" "${BUILD_DIR}/${chosen}"; do
      if [[ -f "${cand}" ]]; then EXE="${cand}"; break; fi
    done
    if [[ -n "${EXE}" ]]; then
      echo "note: --exe-name ${EXE_NAME} does not match CMake OUTPUT_NAME ${chosen}; using the marker" >&2
      EXE_NAME="${chosen}"
    fi
  fi
fi
if [[ -z "${EXE}" ]]; then
  echo "error: game executable '${EXE_NAME}' not found under ${BUILD_DIR}" >&2
  ls -la "${BUILD_DIR}" >&2 || true
  exit 1
fi
EXE_BASENAME="$(basename "${EXE}")"
EXE_DIR="$(dirname "${EXE}")"

# A bundled release is a FULL build. The build publishes which it is: the
# PSX_HAS_GAME_DISPATCH define only exists when generated C was linked.
# Refuse to package a setup host as a game.
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]] &&
   grep -q '^PSXRECOMP_FORCE_SETUP_HOST:BOOL=ON' "${BUILD_DIR}/CMakeCache.txt"; then
  echo "error: ${BUILD_DIR} was configured with PSXRECOMP_FORCE_SETUP_HOST=ON -- that is a setup host, not a game." >&2
  echo "  Reconfigure with -DPSXRECOMP_REQUIRE_GAME_C=ON (generated/ committed) and rebuild." >&2
  exit 1
fi
if ! grep -qs 'PSX_HAS_GAME_DISPATCH' "${BUILD_DIR}/build.ninja" "${BUILD_DIR}/compile_commands.json" \
     "${BUILD_DIR}/CMakeFiles/${RUNTIME_TARGET}.dir/flags.make" 2>/dev/null; then
  echo "error: ${BUILD_DIR} did not compile generated game C (no PSX_HAS_GAME_DISPATCH in the build flags)." >&2
  echo "  A bundled release links the committed generated/ tree; configure with -DPSXRECOMP_REQUIRE_GAME_C=ON." >&2
  exit 1
fi

normalize_ver() { local v; v="$(printf '%s' "${1:-}" | tr -d '[:space:]')"; printf '%s' "${v#v}"; }
BUILT=""
for stamp in "${EXE_DIR}/psx_game_version.txt" "${BUILD_DIR}/psx_game_version.txt" "${BUILD_DIR}/Release/psx_game_version.txt"; do
  if [[ -f "${stamp}" ]]; then
    BUILT="$(normalize_ver "$(cat "${stamp}")")"
    echo "lobby pin stamp: ${stamp} -> ${BUILT}"
    break
  fi
done
if [[ -z "${BUILT}" ]]; then
  echo "error: missing psx_game_version.txt next to ${EXE}" >&2
  echo "  Rebuild with current psxrecomp runtime.cmake so the lobby pin is stamped." >&2
  exit 1
fi
if [[ -n "${REQUESTED}" && "${REQUESTED}" != "${BUILT}" ]]; then
  echo "error: RELEASE_VERSION/VERSION=${REQUESTED} but binary stamp=${BUILT}" >&2
  echo "  Rebuild with -DPSX_GAME_VERSION=${REQUESTED} (or pin VERSION then reconfigure)." >&2
  exit 1
fi
VERSION="${BUILT}"
printf '%s\n' "${VERSION}" >"${ROOT}/VERSION"

DIST="${ROOT:?}/dist"
STAGE="${DIST:?}/stage-game-${ARTIFACT:?}"
ZIP_NAME="${ZIP_PREFIX:?}-${VERSION:?}-${ARTIFACT:?}.zip"
rm -rf "${STAGE:?}"
mkdir -p "${STAGE}" "${DIST}"
rm -f "${DIST:?}/${ZIP_NAME:?}"

# --- The executable and what CMake staged beside it --------------------------
cp -a "${EXE}" "${STAGE}/"
if [[ -f "${EXE_DIR}/psx_game_version.txt" ]]; then
  cp -a "${EXE_DIR}/psx_game_version.txt" "${STAGE}/psx_game_version.txt"
else
  printf '%s\n' "${VERSION}" >"${STAGE}/psx_game_version.txt"
fi

if [[ "${EXE_BASENAME}" == *.exe ]]; then
  shopt -s nullglob
  for dll in "${EXE_DIR}"/*.dll "${EXE_DIR}"/*.DLL; do
    cp -a "${dll}" "${STAGE}/"
    echo "staged sibling DLL $(basename "${dll}")"
  done
  shopt -u nullglob
fi

if [[ ! -d "${EXE_DIR}/assets/fonts" || ! -d "${EXE_DIR}/assets/img" ]]; then
  echo "error: ${EXE_DIR}/assets/{fonts,img} missing -- rebuild ${RUNTIME_TARGET}" >&2
  exit 1
fi
# Everything runtime.cmake staged under <exe>/assets ships as a unit: fonts,
# img, and the window icon it places at assets/psxrecomp.png (APP_ICON).
mkdir -p "${STAGE}/assets"
cp -a "${EXE_DIR}/assets/." "${STAGE}/assets/"
if [[ ! -f "${STAGE}/assets/img/boxart.tga" && -f "${ROOT}/launcher_assets/img/boxart.tga" ]]; then
  cp -a "${ROOT}/launcher_assets/img/boxart.tga" "${STAGE}/assets/img/boxart.tga"
fi

# Bundled OpenBIOS: runtime.cmake stages the exact image the compiled backend
# consumed plus its MIT notice at <exe>/bios/. The runtime resolves
# "bios/openbios.bin" relative to the executable, so ship the directory as a
# unit. Nothing else from bios/ may ship.
if [[ ! -f "${EXE_DIR}/bios/openbios.bin" || ! -f "${EXE_DIR}/bios/OpenBIOS.LICENSE" ]]; then
  echo "error: ${EXE_DIR}/bios/{openbios.bin,OpenBIOS.LICENSE} missing -- the build did not link the OpenBIOS backend" >&2
  exit 1
fi
mkdir -p "${STAGE}/bios"
cp -a "${EXE_DIR}/bios/openbios.bin" "${EXE_DIR}/bios/OpenBIOS.LICENSE" "${STAGE}/bios/"

# Runtime configs: game.toml is what the runtime reads beside the exe; the
# build stages it there, so take the staged copy (it is the packaged config
# the cache tag derives from). game_options.toml is optional.
for cfg in game.toml game_options.toml; do
  if [[ -f "${EXE_DIR}/${cfg}" ]]; then
    cp -a "${EXE_DIR}/${cfg}" "${STAGE}/${cfg}"
  elif [[ -f "${ROOT}/${cfg}" ]]; then
    cp -a "${ROOT}/${cfg}" "${STAGE}/${cfg}"
  fi
done
[[ -f "${STAGE}/game.toml" ]] || { echo "error: no game.toml to ship (build tree or repo root)" >&2; exit 1; }

# Wave-5 F4: without overlay_cache = true the runtime never initialises the
# overlay loader and every streamed overlay runs interpreted for every player.
if ! grep -qE '^[[:space:]]*overlay_cache[[:space:]]*=[[:space:]]*true' "${STAGE}/game.toml"; then
  if [[ -z "${SHIP_WITHOUT_OVERLAY_CACHE_KEY_BECAUSE}" ]]; then
    echo "error: REFUSING TO PACKAGE: ${STAGE}/game.toml has no '[runtime] overlay_cache = true'." >&2
    echo "       Add the key, or pass --ship-without-overlay-cache-key-because '<reason>'." >&2
    exit 1
  fi
  echo "warning: packaging without overlay_cache = true (reason: ${SHIP_WITHOUT_OVERLAY_CACHE_KEY_BECAUSE})" >&2
fi

for rel in "${RUNTIME_FILES[@]+"${RUNTIME_FILES[@]}"}"; do
  [[ -n "${rel}" ]] || continue
  src=""
  for c in "${EXE_DIR}/${rel}" "${ROOT}/${rel}"; do [[ -f "${c}" ]] && { src="${c}"; break; }; done
  [[ -n "${src}" ]] || { echo "error: --runtime-file ${rel} not found (build tree or repo)" >&2; exit 1; }
  mkdir -p "$(dirname "${STAGE}/${rel}")"
  cp -a "${src}" "${STAGE}/${rel}"
done
for name in "${RUNTIME_DIRS[@]+"${RUNTIME_DIRS[@]}"}"; do
  [[ -n "${name}" ]] || continue
  [[ -d "${EXE_DIR}/${name}" ]] || { echo "error: --runtime-dir ${name} missing under ${EXE_DIR}" >&2; exit 1; }
  mkdir -p "${STAGE}/${name}"
  cp -a "${EXE_DIR}/${name}/." "${STAGE}/${name}/"
  echo "staged runtime dir ${name}/"
done

# --- Mod catalog, verified against what THIS build declared -------------------
MANIFEST="$(find "${BUILD_DIR}" -maxdepth 3 -name "psx_mod_catalog_${RUNTIME_TARGET}.txt" -type f 2>/dev/null | head -n1 || true)"
mods_args=(--build-path "${EXE_DIR}" --stage "${STAGE}" --runtime-target "${RUNTIME_TARGET}")
[[ -n "${MANIFEST}" ]] && mods_args+=(--catalog-manifest "${MANIFEST}")
psx_add_mod_catalog "${mods_args[@]}"

prune_dev_mods() {
  if ! "${PY}" "${SCRIPT_DIR}/mod_channel_filter.py" "$@"; then
    echo "error: developer-channel filtering failed" >&2
    exit 1
  fi
}
if [[ "${EXCLUDE_DEV_MODS}" -eq 1 ]]; then
  echo "excluding developer-channel mods from this package"
  prune_dev_mods "${STAGE}/mods/bundled"
  left=$( { grep -rlE '^[[:space:]]*channel[[:space:]]*=[[:space:]]*"developer"[[:space:]]*$' \
      "${STAGE}/mods" --include=manifest.toml 2>/dev/null || true; } | wc -l)
  if [[ "${left}" -ne 0 ]]; then
    echo "error: ${left} developer manifest(s) survived pruning" >&2
    exit 1
  fi
else
  n=$( { grep -rlE '^[[:space:]]*channel[[:space:]]*=[[:space:]]*"developer"[[:space:]]*$' \
      "${STAGE}/mods" --include=manifest.toml 2>/dev/null || true; } | wc -l)
  [[ "${n}" -eq 0 ]] || echo "note: package includes ${n} developer-channel manifest(s); pass --exclude-dev-mods for a public release"
fi

# --- Overlay shard cache (developer-local only) and toolchain -----------------
GAME_BIN=""
for cand in "${RECOMPILER_BUILD}/psxrecomp-game" "${RECOMPILER_BUILD}/psxrecomp-game.exe" "${RECOMPILER_BUILD}/Release/psxrecomp-game.exe"; do
  if [[ -f "${cand}" ]]; then GAME_BIN="${cand}"; break; fi
done

if [[ -n "${OVERLAY_CACHE_ROOT}" ]]; then
  [[ -n "${GAME_BIN}" ]] || { echo "error: --overlay-cache-root needs psxrecomp-game under ${RECOMPILER_BUILD} (cache tag derivation)" >&2; exit 1; }
  game_id="$(sed -n 's/^[[:space:]]*id[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "${STAGE}/game.toml" | head -1)"
  [[ -n "${game_id}" ]] || { echo "error: [game] id missing from the staged game.toml (cache namespace)" >&2; exit 1; }
  cg_tag="$(psx_overlay_cg_tag --runtime-include "${FRAMEWORK}/runtime/include" \
      --recompiler "${GAME_BIN}" --game-toml "${STAGE}/game.toml" \
      --flavor-from-build "${BUILD_DIR}" --runtime-target "${RUNTIME_TARGET}")"
  [[ -n "${cg_tag}" ]] || { echo "error: could not derive the overlay cache tag" >&2; exit 1; }
  psx_add_overlay_cache --game-id "${game_id}" --cache-src-root "${OVERLAY_CACHE_ROOT}" \
      --stage "${STAGE}" --cg-tag "${cg_tag}"
else
  if [[ -z "${SHIP_WITHOUT_OVERLAY_CACHE_BECAUSE}" ]]; then
    echo "error: no --overlay-cache-root given. A shard cache is compiled from the disc, so a" >&2
    echo "       release without one relies on the compiled static shard plus runtime autocompile." >&2
    echo "       State that in words: --ship-without-overlay-cache-because '<reason>'." >&2
    exit 1
  fi
  echo "overlay cache: NONE shipped -- ${SHIP_WITHOUT_OVERLAY_CACHE_BECAUSE}"
fi

if [[ "${STAGE_OVERLAY_TOOLCHAIN}" -eq 1 ]]; then
  [[ -n "${GAME_BIN}" ]] || { echo "error: overlay toolchain needs psxrecomp-game under ${RECOMPILER_BUILD} (pass --recompiler-build, or --no-overlay-toolchain)" >&2; exit 1; }
  case "${ARTIFACT}" in
    windows-*) platform=win ;;
    linux-*)   platform=linux ;;
    macos-arm64|macos-aarch64) platform=macos-arm64 ;;
    macos-x64|macos-x86_64)   platform=macos-x64 ;;
    *) echo "error: cannot map artifact '${ARTIFACT}' to an overlay toolchain platform" >&2; exit 1 ;;
  esac
  tc_args=(--stage "${STAGE}" --recomp-dir "$(dirname "${GAME_BIN}")" \
           --recomp-tools "${FRAMEWORK}/tools" --recomp-include "${FRAMEWORK}/runtime/include" \
           --dl-cache "${DIST}/.dl-cache" --platform "${platform}")
  [[ "${platform}" == win ]] && tc_args+=(--mingw-bin "${RUNTIME_BIN_DIR}")
  psx_add_overlay_toolchain "${tc_args[@]}"
fi

# --- Notices and docs ---------------------------------------------------------
mkdir -p "${STAGE}/licenses"
if [[ -d "${FRAMEWORK}/runtime/licenses" ]]; then
  cp -a "${FRAMEWORK}/runtime/licenses/." "${STAGE}/licenses/"
fi
[[ -f "${FRAMEWORK}/THIRD_PARTY_ATTRIBUTION.md" ]] && cp -a "${FRAMEWORK}/THIRD_PARTY_ATTRIBUTION.md" "${STAGE}/licenses/"
[[ -f "${FRAMEWORK}/LICENSE" ]] && cp -a "${FRAMEWORK}/LICENSE" "${STAGE}/licenses/psxrecomp-LICENSE"
# recomp-ui: the executable links it (MIT) and assets/ carries its fonts and
# images (OFL-1.1, CC BY-SA 4.0), so its license and asset notices ship too.
# The build's RECOMP_UI_ROOT says which tree it linked; <root>/recomp-ui is the
# New Project Layout default.
if ! grep -qs '^PSX_RECOMP_UI:BOOL=OFF' "${BUILD_DIR}/CMakeCache.txt"; then
  UI_ROOT="$(sed -n 's/^RECOMP_UI_ROOT:[A-Z]*=//p' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | head -n1 || true)"
  [[ -n "${UI_ROOT}" && -f "${UI_ROOT}/recomp_ui.cmake" ]] || UI_ROOT="${ROOT}/recomp-ui"
  [[ -f "${UI_ROOT}/recomp_ui.cmake" ]] || { echo "error: recomp-ui assets are staged but no recomp-ui tree found (RECOMP_UI_ROOT / ${ROOT}/recomp-ui)" >&2; exit 1; }
  [[ -f "${UI_ROOT}/LICENSE" ]] || { echo "error: ${UI_ROOT}/LICENSE missing" >&2; exit 1; }
  cp -a "${UI_ROOT}/LICENSE" "${STAGE}/licenses/recomp-ui-LICENSE"
  for sub in fonts img; do
    notice="${UI_ROOT}/assets/common/${sub}/NOTICE.md"
    [[ -f "${notice}" ]] || { echo "error: ${notice} missing" >&2; exit 1; }
    cp -a "${notice}" "${STAGE}/assets/${sub}/NOTICE.md"
  done
fi
if [[ -z "$(ls -A "${STAGE}/licenses" 2>/dev/null)" ]]; then
  echo "error: no third-party notices staged (${FRAMEWORK}/runtime/licenses empty?)" >&2
  exit 1
fi
for d in "${DOCS[@]+"${DOCS[@]}"}"; do
  [[ -n "${d}" ]] || continue
  [[ -f "${ROOT}/${d}" ]] || { echo "error: --doc ${d} not found" >&2; exit 1; }
  cp -a "${ROOT}/${d}" "${STAGE}/$(basename "${d}")"
done
mkdir -p "${STAGE}/saves"

cat >"${STAGE}/README.txt" <<EOF
${DISPLAY_NAME} ${VERSION}
Platform: ${ARTIFACT}

This is the compiled game. Nothing needs to be generated or built.

1. Run ${EXE_BASENAME}.
2. On first run, point it at ${DISC_HINT}. The disc is never copied here.
3. Play. Saves land in saves/; settings beside the executable.

The game runs on the bundled OpenBIOS (bios/, MIT licensed). No retail BIOS
is included or required. Third-party notices are in licenses/.

overlay_toolchain/ lets the game turn code it streams from your disc into
native code on this machine; it is not a build kit and needs no action.
EOF

# --- Gates: what must NOT be in a bundled release -------------------------------
forbidden=()
for p in psxrecomp recomp-ui generated seeds CMakeLists.txt codegen_setup.c codegen_setup.h \
         psxrecomp_cli.py psxrecomp-game psxrecomp-game.exe psxrecomp-bios psxrecomp-bios.exe \
         toolchain disc src; do
  [[ -e "${STAGE}/${p}" ]] && forbidden+=("${p}")
done
if [[ "${#forbidden[@]}" -gt 0 ]]; then
  echo "error: bundled release stage contains build-kit content:" >&2
  printf '  %s\n' "${forbidden[@]}" >&2
  exit 1
fi
# BIOS *profiles* (SCPH1001.toml inside overlay_toolchain/bios) are text the
# toolchain needs; only image files are forbidden.
bad_bios="$(find "${STAGE}" -type f \( \( -iname 'SCPH*' -a ! -iname '*.toml' \) -o -iname '*.bin' \) \
    ! -path "${STAGE}/bios/openbios.bin" 2>/dev/null || true)"
if [[ -n "${bad_bios}" ]]; then
  echo "error: BIOS image(s) other than the bundled OpenBIOS in the stage:" >&2
  printf '%s\n' "${bad_bios}" | sed 's/^/  /' >&2
  exit 1
fi
bad_disc="$(find "${STAGE}" -type f \( -iname '*.cue' -o -iname '*.iso' -o -iname '*.img' -o -iname '*.chd' -o -iname '*.mcd' -o -iname '*.mcr' \) 2>/dev/null || true)"
if [[ -n "${bad_disc}" ]]; then
  echo "error: disc / memory-card files in the stage:" >&2
  printf '%s\n' "${bad_disc}" | sed 's/^/  /' >&2
  exit 1
fi
if find "${STAGE}" -type f -name '*.c' ! -path "${STAGE}/overlay_toolchain/*" | grep -q .; then
  echo "error: C sources in the stage outside overlay_toolchain/:" >&2
  find "${STAGE}" -type f -name '*.c' ! -path "${STAGE}/overlay_toolchain/*" | sed 's/^/  /' >&2
  exit 1
fi

# --- Windows: DLLs beside the exe, then Authenticode -------------------------
if [[ "${EXE_BASENAME}" == *.exe ]]; then
  BUNDLE="${SCRIPT_DIR}/bundle_mingw_dlls.sh"
  [[ -f "${BUNDLE}" ]] || { echo "error: missing ${BUNDLE}" >&2; exit 1; }
  bash "${BUNDLE}" \
    --runtime-bin "${RUNTIME_BIN_DIR}" --search-dir "${EXE_DIR}" --search-dir "${BUILD_DIR}" \
    --exe "${STAGE}/${EXE_BASENAME}" --dest "${STAGE}" --label "${EXE_BASENAME}" \
    --require libgcc_s_seh-1.dll --require libstdc++-6.dll --require libwinpthread-1.dll \
    --require libssp-0.dll --require zlib1.dll --require z.dll
  SIGN_SH="${SCRIPT_DIR}/ci/sign_windows.sh"
  if [[ -f "${SIGN_SH}" ]]; then
    bash "${SIGN_SH}" "${STAGE}"
  else
    echo "note: ${SIGN_SH} missing; Windows binaries ship unsigned" >&2
  fi
fi

find "${STAGE}" -exec touch -c {} + 2>/dev/null || find "${STAGE}" -exec touch {} +

"${PY}" "${SCRIPT_DIR}/create_release_zip.py" --source "${STAGE}" --output "${DIST}/${ZIP_NAME}" >/dev/null
echo "Wrote ${DIST}/${ZIP_NAME}"
du -h "${DIST}/${ZIP_NAME}"
