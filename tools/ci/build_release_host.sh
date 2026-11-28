#!/usr/bin/env bash
set -euo pipefail
EXTRA=()
if [[ -n "${CMAKE_OSX_DEPLOYMENT_TARGET:-}" ]]; then
  EXTRA+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
fi
if [[ "$(uname -s)" != MINGW* && "$(uname -s)" != MSYS* ]] && command -v ccache >/dev/null 2>&1; then
  EXTRA+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache")
  EXTRA+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi
if [[ "$(uname -s)" == "Darwin" ]]; then
  BREW_PREFIX="$(brew --prefix)"
  export PATH="${BREW_PREFIX}/bin${PATH:+:${PATH}}"
  if command -v brew >/dev/null 2>&1; then
    SHADERC_PREFIX="$(brew --prefix shaderc 2>/dev/null || true)"
    VK_HDR_PREFIX="$(brew --prefix vulkan-headers 2>/dev/null || true)"
    if [[ -n "${SHADERC_PREFIX}" && -d "${SHADERC_PREFIX}/bin" ]]; then
      export PATH="${SHADERC_PREFIX}/bin:${PATH}"
    fi
    for p in "${VK_HDR_PREFIX}" "${SHADERC_PREFIX}" "${BREW_PREFIX}"; do
      if [[ -n "${p}" && -d "${p}" ]]; then
        export CMAKE_PREFIX_PATH="${p}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
      fi
    done
  else
    export CMAKE_PREFIX_PATH="${BREW_PREFIX}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
  fi
  if [[ -f "${BREW_PREFIX}/include/vulkan/vulkan.h" ]]; then
    export VULKAN_SDK="${BREW_PREFIX}"
  elif [[ -n "${VK_HDR_PREFIX:-}" && -f "${VK_HDR_PREFIX}/include/vulkan/vulkan.h" ]]; then
    export VULKAN_SDK="${VK_HDR_PREFIX}"
  fi
elif [[ "$(uname -s)" == "Linux" ]]; then
  if [[ -z "${VULKAN_SDK:-}" && -f /usr/include/vulkan/vulkan.h ]]; then
    export VULKAN_SDK=/usr
  fi
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  if [[ -z "${VULKAN_SDK:-}" && -f /mingw64/include/vulkan/vulkan.h ]]; then
    export VULKAN_SDK=/mingw64
  fi
fi
if command -v glslc >/dev/null 2>&1; then
  EXTRA+=("-DGLSLC_EXE=$(command -v glslc)")
fi
JOBS="${PSX_RELEASE_JOBS:-2}"
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  unset PSXRECOMP_TOOLCHAIN_DIR TOOLCHAIN_DIR BPE_TOOLCHAIN_DIR RETCOMM_TOOLCHAIN_DIR || true
  unset CMAKE_PREFIX_PATH SDL3_DIR ZLIB_ROOT || true
fi
if [[ -n "${PSX_SDL3_SOURCE_DIR:-}" && -f "${PSX_SDL3_SOURCE_DIR}/CMakeLists.txt" ]]; then
  EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_SDL3=${PSX_SDL3_SOURCE_DIR}")
elif [[ -f "${GITHUB_WORKSPACE}/.cache/sdl3-src/CMakeLists.txt" ]]; then
  EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_SDL3=${GITHUB_WORKSPACE}/.cache/sdl3-src")
fi
case "${PSX_PACKAGE_KIND}" in setup|prebuilt) ;; *) echo 'Invalid package kind' >&2; exit 1;; esac
if [[ "${PSX_PACKAGE_KIND}" == "setup" ]]; then
  EXTRA+=(-DPSXRECOMP_FORCE_SETUP_HOST=ON -DPSXRECOMP_ALLOW_NO_BIOS=ON)
else
  EXTRA+=(-DPSXRECOMP_REQUIRE_GAME_C=ON -DPSXRECOMP_BIOS_STALE_FATAL=ON
          -DPSXRECOMP_FORCE_SETUP_HOST=OFF -DPSXRECOMP_ALLOW_NO_BIOS=OFF)
fi
while IFS= read -r arg; do
  [[ -z "${arg}" ]] || EXTRA+=("${arg}")
done <<< "${PSX_EXTRA_CMAKE_ARGS:-}"
cmake -S . -B build-ci -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSX_SETUP_WIZARD=ON -DPSX_EXECUTION_PROFILE=ENHANCED \
  -DPSX_GAME_VERSION="${RELEASE_VERSION}" \
  -DPSX_RELEASE_CHANNEL="${PSX_RELEASE_CHANNEL}" \
  "${EXTRA[@]}" 2>&1 | tee build-ci-configure.log
if [[ "${PSX_PACKAGE_KIND}" == "prebuilt" ]]; then
  grep -q 'linking generated game C (full runtime)' build-ci-configure.log
  grep -qE 'BIOS backends linked: .*OpenBIOS' build-ci-configure.log
  if [[ "${PSX_REQUIRE_RETAIL_BACKEND}" == "true" ]]; then
    grep -qE 'BIOS backends linked: .*SCPH1001' build-ci-configure.log
  fi
fi
if ! grep -q 'Vulkan backend: headers' build-ci-configure.log; then
  echo "::error::CMake did not enable PSX_HAVE_VULKAN (see configure log)"
  exit 1
fi
DEFINES=(PSX_HAVE_VULKAN)
if [[ "${PSX_PACKAGE_KIND}" == "prebuilt" ]]; then DEFINES+=(PSX_HAS_GAME_DISPATCH); fi
for define in "${DEFINES[@]}"; do
  FOUND=0
  for f in \
      build-ci/build.ninja \
      build-ci/compile_commands.json \
      build-ci/CMakeFiles/psx-runtime.dir/flags.make; do
    if [[ -f "${f}" ]] && grep -q "${define}" "${f}"; then
      FOUND=1
      echo "Found ${define} in ${f}"
      break
    fi
  done
  if [[ "${FOUND}" -ne 1 ]]; then
    echo "::error::${define} missing from psx-runtime compile flags"
    exit 1
  fi
done
cmake --build build-ci --target psx-runtime -j"${JOBS}"
STAMP=""
for cand in \
    build-ci/psx_game_version.txt \
    build-ci/Release/psx_game_version.txt \
    build-ci/*/psx_game_version.txt
do
  if [[ -f "${cand}" ]]; then STAMP="${cand}"; break; fi
done
if [[ -z "${STAMP}" ]]; then
  STAMP="$(find build-ci -name psx_game_version.txt -type f 2>/dev/null | head -n1 || true)"
fi
if [[ -z "${STAMP}" || ! -f "${STAMP}" ]]; then
  echo "::error::missing psx_game_version.txt after build (update psxrecomp submodule)"
  exit 1
fi
BUILT="$(tr -d '[:space:]' <"${STAMP}")"
BUILT="${BUILT#v}"
if [[ "${BUILT}" != "${RELEASE_VERSION}" ]]; then
  echo "::error::lobby pin stamp ${BUILT} != RELEASE_VERSION ${RELEASE_VERSION} (${STAMP})"
  exit 1
fi
echo "lobby pin ok: ${BUILT} (${STAMP})"
