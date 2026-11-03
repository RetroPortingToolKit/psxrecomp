include_guard(GLOBAL)
option(PSX_NATIVE_CAPTURE_JOBS "Reuse raw snapshot capture allocations" ON)

function(psxrecomp_capture_jobs target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_ring FALSE)
    if(PSXRECOMP_HAS_RBENGINE OR PSXRECOMP_HAS_RBENGINE_SNAP)
        set(_ring TRUE)
    endif()
    set(_selected 0)
    if(PSX_NATIVE_CAPTURE_JOBS AND _profile STREQUAL "ENHANCED")
        set(_selected 1)
    endif()
    if(_selected AND _ring)
        file(STRINGS "${RECOMP_RBENGINE_ROOT}/include/retcomm_rbengine/snap_ring.h"
            _api REGEX "^#define RBE_SNAP_CAPTURE_API_VERSION 1$")
        if(NOT _api)
            message(FATAL_ERROR "Native capture jobs need rbengine reusable capture API v1")
        endif()
    endif()
    get_target_property(_sources ${target} SOURCES)
    set(_common "")
    set(_original "")
    foreach(_source IN LISTS _sources)
        get_filename_component(_name "${_source}" NAME)
        if(_name STREQUAL "boot_state.c")
            list(APPEND _original "${_source}")
        else()
            list(APPEND _common "${_source}")
        endif()
    endforeach()
    list(LENGTH _original _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Native capture jobs require exactly one snapshot body")
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES "${_common}")
    set(_native ${_original})
    if(PSX_NATIVE_CAPTURE_JOBS)
        set(_native "${PSXRECOMP_ROOT}/runtime/src/boot_state_capture_jobs.c")
        if(_ring)
            list(APPEND _native "${PSXRECOMP_ROOT}/runtime/src/capture_ring.c")
        endif()
    endif()
    set(_ring_contract "")
    if(_ring)
        set(_ring_contract "${RECOMP_RBENGINE_ROOT}/include/retcomm_rbengine/snap_ring.h"
                           "${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c")
    endif()
    psxrecomp_add_implementation(${target}
        NAME snapshot-capture CONTRACT reusable-raw-capture-v1
        HLE_SOURCES ${_native} LLE_SOURCES ${_original}
        CONTRACT_FILES ${_original}
            "${PSXRECOMP_ROOT}/runtime/include/boot_state.h"
            "${PSXRECOMP_ROOT}/runtime/include/pst_wire.h"
            "${PSXRECOMP_ROOT}/runtime/include/capture_ring.h"
            "${PSXRECOMP_ROOT}/runtime/src/psx_rewind.c"
            "${PSXRECOMP_ROOT}/runtime/src/netplay_snap_ring.c"
            ${_ring_contract}
            "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_CAPTURE_JOBS=${_selected})
endfunction()
