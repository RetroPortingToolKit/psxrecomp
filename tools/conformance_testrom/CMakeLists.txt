cmake_minimum_required(VERSION 3.20)
project(PSXConformanceTestROM C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(_default_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PSXRECOMP_ROOT "${_default_root}" CACHE PATH "Path to PSXRecomp")
set(CONFORMANCE_RUN_DIR "" CACHE PATH "Prepared conformance fixture directory")
set(CONFORMANCE_DEBUG_PORT "4601" CACHE STRING "Runtime debug port")

# The headless conformance binary does not expose product UI features. Keep
# optional runtime integrations out so a clean fixture build needs no unrelated
# private/submodule state.
set(PSX_REWIND OFF CACHE BOOL "Disable rewind in conformance fixture" FORCE)
set(PSX_NETPLAY OFF CACHE BOOL "Disable netplay in conformance fixture" FORCE)
set(PSX_RECOMP_UI OFF CACHE BOOL "Disable launcher UI in conformance fixture" FORCE)
set(PSX_DEBUG_TOOLS ON CACHE BOOL "Enable the structured oracle endpoint" FORCE)

if(NOT CONFORMANCE_RUN_DIR)
    message(FATAL_ERROR "Set -DCONFORMANCE_RUN_DIR=<prepared fixture directory>")
endif()
get_filename_component(CONFORMANCE_RUN_DIR "${CONFORMANCE_RUN_DIR}" ABSOLUTE)

file(GLOB _game_full "${CONFORMANCE_RUN_DIR}/generated/test.exe_full_*.c")
if(NOT _game_full AND EXISTS "${CONFORMANCE_RUN_DIR}/generated/test.exe_full.c")
    set(_game_full "${CONFORMANCE_RUN_DIR}/generated/test.exe_full.c")
endif()
set(_game_dispatch "${CONFORMANCE_RUN_DIR}/generated/test.exe_dispatch.c")
if(NOT _game_full OR NOT EXISTS "${_game_dispatch}")
    message(FATAL_ERROR "Fixture has not been recompiled: ${CONFORMANCE_RUN_DIR}")
endif()

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

psxrecomp_add_runtime_target(psx-conformance
    GAME_GENERATED_FULL_C ${_game_full}
    GAME_GENERATED_DISPATCH_C "${_game_dispatch}"
    DEBUG_PORT "${CONFORMANCE_DEBUG_PORT}"
    WINDOW_TITLE "PSX Conformance"
    DEFAULT_GAME_CONFIG_PATH "${CONFORMANCE_RUN_DIR}/game.toml"
    EXE_NAME "PSXConformance"
)
