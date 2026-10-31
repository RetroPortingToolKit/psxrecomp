include_guard(GLOBAL)
option(PSX_NATIVE_MEDIA_JOBS "Use bounded native MDEC transfer jobs in ENHANCED builds" ON)

function(psxrecomp_media_jobs target)
    get_target_property(_sources ${target} SOURCES)
    set(_common "")
    set(_dma "")
    foreach(_source IN LISTS _sources)
        get_filename_component(_name "${_source}" NAME)
        if(_name STREQUAL "dma.c")
            if(_dma)
                message(FATAL_ERROR "Duplicate DMA body for ${target}")
            endif()
            set(_dma "${_source}")
        else()
            list(APPEND _common "${_source}")
        endif()
    endforeach()
    if(NOT _dma)
        message(FATAL_ERROR "MDEC media jobs require the maintained DMA body")
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES "${_common}")
    set(_native "${_dma}")
    if(PSX_NATIVE_MEDIA_JOBS)
        set(_native "${PSXRECOMP_ROOT}/runtime/src/dma_media_jobs.c")
    endif()
    psxrecomp_add_implementation(${target}
        NAME mdec-media-jobs CONTRACT bounded-fifo-publication-v1
        HLE_SOURCES "${_native}" LLE_SOURCES "${_dma}"
        CONTRACT_FILES "${_dma}"
                       "${PSXRECOMP_ROOT}/runtime/include/dma.h"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
