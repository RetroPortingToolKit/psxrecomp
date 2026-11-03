include_guard(GLOBAL)

# Reuse the exact store-counter induction engine for title-audited wait loops.
# Selection is linked into the executable and its execution identity. No runtime
# mode, new loop detector, or speculative guest implementation is introduced.
function(psxrecomp_native_idle_wait target)
    set(_sites ${ARGN})
    list(REMOVE_DUPLICATES _sites)
    list(LENGTH _sites _count)
    if(_count GREATER 16)
        message(FATAL_ERROR "NATIVE_IDLE_WAIT_SITES supports at most 16 audited anchors")
    endif()
    set(_values "")
    foreach(_pc IN LISTS _sites)
        if(NOT _pc MATCHES "^(0[xX][0-9A-Fa-f]+|[0-9]+)$")
            message(FATAL_ERROR "Invalid NATIVE_IDLE_WAIT_SITES address: ${_pc}")
        endif()
        math(EXPR _value "${_pc}")
        math(EXPR _physical "${_value} & 0x1FFFFFFF")
        math(EXPR _alignment "${_value} & 3")
        if(_value LESS 0 OR _value GREATER 4294967295 OR
           _physical GREATER_EQUAL 8388608 OR NOT _alignment EQUAL 0)
            message(FATAL_ERROR "NATIVE_IDLE_WAIT_SITES needs aligned main-RAM PCs: ${_pc}")
        endif()
        string(APPEND _values "${_value}u,")
    endforeach()
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/native-idle-wait/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    set(_prefix "#include \"psx_cycles.h\"\n#include \"psx_native_idle_wait.h\"\n")
    if(NOT _sites)
        file(CONFIGURE OUTPUT "${_directory}/unbound.c" CONTENT
            "${_prefix}int psx_build_idle_wait_configure(void) { return -1; }\n" @ONLY)
        target_sources(${target} PRIVATE "${_directory}/unbound.c")
        return()
    endif()
    file(CONFIGURE OUTPUT "${_directory}/native.c" CONTENT
        "${_prefix}int psx_build_idle_wait_configure(void) {\nstatic const uint32_t sites[] = {${_values}};\ng_idle_skip_enabled = 1;\ng_idle_skip_ext = 1;\npsx_idle_skip_set_store_counter_sites(sites, ${_count});\npsx_idle_skip_set_audited_only(1);\nreturn ${_count};\n}\n" @ONLY)
    file(CONFIGURE OUTPUT "${_directory}/reference.c" CONTENT
        "${_prefix}int psx_build_idle_wait_configure(void) {\ng_idle_skip_enabled = 0;\ng_idle_skip_ext = 0;\npsx_idle_skip_set_store_counter_sites(0, 0);\npsx_idle_skip_set_audited_only(0);\nreturn 0;\n}\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME idle-wait CONTRACT audited-store-counter-v2
        HLE_SOURCES "${_directory}/native.c"
        LLE_SOURCES "${_directory}/reference.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/src/psx_cycles.c"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_cycles.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_native_idle_wait.h")
endfunction()
