include_guard(GLOBAL)
option(PSX_BLOCK_AUDIO "Batch audio between observable SPU boundaries" OFF)

function(psxrecomp_block_audio target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_BLOCK_AUDIO)
        set(_native 1)
    endif()
    get_target_property(_sources ${target} SOURCES)
    set(_common "")
    set(_interrupts "")
    foreach(_source IN LISTS _sources)
        get_filename_component(_name "${_source}" NAME)
        if(_name STREQUAL "interrupts.c")
            set(_interrupts "${_source}")
        else()
            list(APPEND _common "${_source}")
        endif()
    endforeach()
    if(NOT _interrupts)
        message(FATAL_ERROR "Block audio requires the maintained interrupt scheduler")
    endif()
    set(_selected "${_interrupts}")
    if(_native)
        set(_selected "${PSXRECOMP_ROOT}/runtime/src/interrupts_block_audio.c")
    endif()
    set_property(TARGET ${target} PROPERTY SOURCES "${_common}")
    psxrecomp_add_implementation(${target}
        NAME block-audio CONTRACT latched-irq-block-${_native}-v1
        HLE_SOURCES "${_selected}" LLE_SOURCES "${_interrupts}"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/src/spu.c"
                       "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
                       "${PSXRECOMP_ROOT}/runtime/src/interrupts_block_audio.c"
                       "${PSXRECOMP_ROOT}/runtime/src/psx_cycles.c"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
