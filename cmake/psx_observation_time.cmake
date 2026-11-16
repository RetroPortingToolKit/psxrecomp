include_guard(GLOBAL)
option(PSX_OBSERVATION_TIME "Service machine time at declared observations" OFF)

function(psxrecomp_observation_time target)
    cmake_parse_arguments(P "" "" "GENERATED_SOURCES;WAIT_SITES" ${ARGN})
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_OBSERVATION_TIME)
        if(NOT PSX_NATIVE_CPU_SPANS)
            message(FATAL_ERROR "Observation time requires native CPU spans")
        endif()
        set(_native 1)
        foreach(_source IN LISTS P_GENERATED_SOURCES)
            get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(NOT EXISTS "${_source}")
                message(FATAL_ERROR "Observation time requires materialized regenerated game sources")
            endif()
            file(READ "${_source}" _text LIMIT 2048)
            if(NOT _text MATCHES "psx-observation-edge-v1")
                message(FATAL_ERROR "Regenerate game sources with observation block boundaries: ${_source}")
            endif()
        endforeach()
        target_compile_definitions(${target} PRIVATE PSX_OBSERVATION_TIME=1)
    endif()
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/observation-time/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    set(_wait "0")
    set(_wait_values "0u")
    set(_wait_count 0)
    foreach(_pc IN LISTS P_WAIT_SITES)
        # Addresses have already passed the native-idle-wait binding validator.
        math(EXPR _value "${_pc}")
        string(APPEND _wait " || ((pc) == ${_value}u)")
        if(_wait_count EQUAL 0)
            set(_wait_values "${_value}u")
        else()
            string(APPEND _wait_values ", ${_value}u")
        endif()
        math(EXPR _wait_count "${_wait_count} + 1")
    endforeach()
    file(CONFIGURE OUTPUT "${_directory}/psx_observation_config.h" CONTENT
        "#pragma once\n#define PSX_OBSERVATION_WAIT_SITE(pc) (${_wait})\n#define PSX_OBSERVATION_WAIT_SITE_VALUES ${_wait_values}\n#define PSX_OBSERVATION_WAIT_SITE_COUNT ${_wait_count}u\n" @ONLY)
    target_include_directories(${target} PRIVATE "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/native.c" CONTENT
        "const unsigned psx_build_observation_time_model = ${_native}u;\n" @ONLY)
    file(CONFIGURE OUTPUT "${_directory}/reference.c" CONTENT
        "const unsigned psx_build_observation_time_model = 0u;\n" @ONLY)
    if(_native)
        get_target_property(_sources ${target} SOURCES)
        set(_selected "")
        set(_found 0)
        foreach(_source IN LISTS _sources)
            get_filename_component(_name "${_source}" NAME)
            if(_name STREQUAL "spu.c")
                list(APPEND _selected "${PSXRECOMP_ROOT}/runtime/src/spu_observation_time.c")
                set(_found 1)
            else()
                list(APPEND _selected "${_source}")
            endif()
        endforeach()
        if(NOT _found)
            message(FATAL_ERROR "Observation time requires the maintained SPU implementation")
        endif()
        set_property(TARGET ${target} PROPERTY SOURCES "${_selected}")
    endif()
    psxrecomp_add_implementation(${target}
        NAME machine-time CONTRACT per-device-observation-${_native}-v3
        HLE_SOURCES "${_directory}/native.c"
        LLE_SOURCES "${_directory}/reference.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/src/psx_cycles.c"
                       "${PSXRECOMP_ROOT}/runtime/src/psx_device_observation.inc"
                       "${PSXRECOMP_ROOT}/runtime/src/cdrom.c"
                       "${PSXRECOMP_ROOT}/runtime/include/cdrom.h"
                       "${PSXRECOMP_ROOT}/runtime/src/memory.c"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_cycles.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_cycle_freeze.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_block_edge.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_observation_view.h"
                       "${PSXRECOMP_ROOT}/runtime/include/overlay_api.h"
                       "${PSXRECOMP_ROOT}/runtime/include/overlay_dispatch_preamble.c.inc"
                       "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
                       "${PSXRECOMP_ROOT}/runtime/src/overlay_loader.c"
                       "${PSXRECOMP_ROOT}/runtime/src/spu_observation_time.c"
                       "${PSXRECOMP_ROOT}/runtime/src/spu.c"
                       "${_directory}/psx_observation_config.h"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
