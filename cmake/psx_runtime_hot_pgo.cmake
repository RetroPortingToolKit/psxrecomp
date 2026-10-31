# Optional project hook used by tools/runtime_hot_pgo.py. Normal builds do not
# include it. It changes compiler policy for three source files, not runtime ABI.
include_guard(GLOBAL)
if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR "Selective runtime PGO needs CMake 3.19 or newer")
endif()
set(PSX_RUNTIME_HOT_PGO_MODE baseline CACHE STRING "baseline, generate, or use")
set(PSX_RUNTIME_HOT_PGO_TARGET psx-runtime CACHE STRING "One selected runtime target")

function(_psxrecomp_runtime_hot_pgo)
    set(_target "${PSX_RUNTIME_HOT_PGO_TARGET}")
    if(NOT TARGET "${_target}")
        message(FATAL_ERROR "Selective PGO target does not exist: ${_target}")
    endif()
    if(NOT PSX_PGO STREQUAL "")
        message(FATAL_ERROR "Selective runtime PGO cannot be combined with global PSX_PGO")
    endif()
    if(PSX_DEBUG_TOOLS OR PSX_RUNTIME_IPO)
        message(FATAL_ERROR "Selective runtime PGO requires the measured production DEBUG_OFF / IPO_OFF configuration")
    endif()
    if(PSX_RUNTIME_HOT_PGO_MODE STREQUAL "baseline")
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
       NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "The selective PGO workflow currently supports GCC; use the ordinary non-PGO build on another compiler")
    endif()
    if(NOT PSX_RUNTIME_HOT_PGO_MODE STREQUAL "generate" AND
       NOT PSX_RUNTIME_HOT_PGO_MODE STREQUAL "use")
        message(FATAL_ERROR "Unknown selective PGO mode: ${PSX_RUNTIME_HOT_PGO_MODE}")
    endif()
    set(_profile "${CMAKE_BINARY_DIR}/runtime-hot-pgo/${_target}/data")
    # MinGW records an absolute object filename with native Windows separators.
    # GCC compares this prefix literally; forward slashes leave the whole drive
    # path in the profile filename and libgcov cannot create its directories.
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _prefix)
    if(PSX_RUNTIME_HOT_PGO_MODE STREQUAL "generate")
        set(_flags "-fprofile-generate=${_profile}" "-fprofile-prefix-path=${_prefix}" "-fprofile-update=atomic")
        target_link_options("${_target}" PRIVATE "-fprofile-generate=${_profile}")
    else()
        set(_flags "-fprofile-use=${_profile}" "-fprofile-prefix-path=${_prefix}"
            -fprofile-partial-training -Werror=missing-profile -Werror=coverage-mismatch)
    endif()
    # SOURCE options are directory-scoped. Evaluate the owner target at
    # generation time so PGXP siblings, oracle and test targets sharing these
    # files retain their ordinary compiler flags and do not need these profiles.
    set(_selected "$<STREQUAL:$<TARGET_PROPERTY:NAME>,${_target}>")
    foreach(_file runtime/src/pgxp.cpp runtime/src/psx_cycles.c runtime/src/memory.c)
        if(NOT EXISTS "${PSXRECOMP_ROOT}/${_file}")
            message(FATAL_ERROR "Selected runtime source is missing: ${_file}")
        endif()
        foreach(_flag IN LISTS _flags)
            set_property(SOURCE "${PSXRECOMP_ROOT}/${_file}" TARGET_DIRECTORY "${_target}" APPEND PROPERTY
                COMPILE_OPTIONS "$<${_selected}:${_flag}>")
        endforeach()
    endforeach()
endfunction()
cmake_language(DEFER CALL _psxrecomp_runtime_hot_pgo)
