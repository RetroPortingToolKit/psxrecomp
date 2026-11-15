include_guard(GLOBAL)
option(PSX_NATIVE_FRAME_SUBMISSION "Submit ENHANCED GPU lists as one native operation" ON)

function(psxrecomp_frame_submission target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_FRAME_SUBMISSION)
        set(_native 1)
    endif()
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_FRAME_SUBMISSION=${_native})
    # Both paths share the GPU command implementations. The selected native
    # list service and its build selection still belong in simulation identity.
    get_target_property(_sources ${target} SOURCES)
    set(_common "")
    set(_gpu "")
    foreach(_source IN LISTS _sources)
        get_filename_component(_name "${_source}" NAME)
        if(_name STREQUAL "gpu.c")
            set(_gpu "${_source}")
        else()
            list(APPEND _common "${_source}")
        endif()
    endforeach()
    if(NOT _gpu)
        message(FATAL_ERROR "Native frame submission requires the maintained GPU body")
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES "${_common}")
    psxrecomp_add_implementation(${target}
        NAME gpu-frame-submission CONTRACT native-list-${_native}-v1
        HLE_SOURCES "${_gpu}" LLE_SOURCES "${_gpu}"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/src/dma.c"
                       "${PSXRECOMP_ROOT}/runtime/include/dma.h"
                       "${PSXRECOMP_ROOT}/runtime/include/gpu.h"
                       "${PSXRECOMP_ROOT}/runtime/include/mod_memory.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_memory.h"
                       "${PSXRECOMP_ROOT}/runtime/include/dma_host_draw.h"
                       "${PSXRECOMP_ROOT}/runtime/include/dma_gpu_ll.h"
                       "${PSXRECOMP_ROOT}/runtime/src/dma_gpu_ll.c"
                       "${PSXRECOMP_ROOT}/runtime/include/ws_prepass_guard.h"
                       "${PSXRECOMP_ROOT}/runtime/include/ws_ui_group.h"
                       "${PSXRECOMP_ROOT}/runtime/src/ws_ui_group.c"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
