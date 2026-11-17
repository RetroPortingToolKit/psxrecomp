include_guard(GLOBAL)
option(PSX_PGXP_OPERATIONS "Guard whole integer precision operations at explicit boundaries" OFF)
option(PSX_PGXP_DEMAND_REGIONS "Compile reconstructible integer precision at observation boundaries" OFF)
function(psxrecomp_precision_operations target)
    cmake_parse_arguments(P "" "" "GENERATED_SOURCES" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unexpected precision-operation arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_selected 0)
    set(_generated "")
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_CPU_SPANS AND PSX_PGXP_OPERATIONS)
        set(_selected 1)
        foreach(_source IN LISTS P_GENERATED_SOURCES)
            get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(NOT EXISTS "${_source}")
                message(FATAL_ERROR "Precision operations require current generated bodies: ${_source}")
            endif()
            file(READ "${_source}" _text LIMIT 2048)
            if(NOT _text MATCHES "psx-pgxp-operations-v2")
                message(FATAL_ERROR "Regenerate game sources before selecting PSX_PGXP_OPERATIONS: ${_source}")
            endif()
        endforeach()
        set(_generated ${P_GENERATED_SOURCES})
    endif()
    target_compile_definitions(${target} PRIVATE PSX_PGXP_OPERATIONS=${_selected})
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/precision-operations/${target}")
    file(MAKE_DIRECTORY "${_dir}")
    file(CONFIGURE OUTPUT "${_dir}/selected.c" CONTENT
        "const unsigned psx_build_precision_operations = ${_selected}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME precision-operations CONTRACT write-owned-integer-operations-${_selected}-v2
        HLE_SOURCES "${_dir}/selected.c" LLE_SOURCES "${_dir}/selected.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/pgxp_operation.h"
            "${PSXRECOMP_ROOT}/runtime/include/pgxp.h"
            "${PSXRECOMP_ROOT}/runtime/include/psx_native_span.h"
            "${PSXRECOMP_ROOT}/runtime/include/psx_direct_memory.h"
            "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
            "${PSXRECOMP_ROOT}/runtime/src/memory.c"
            "${PSXRECOMP_ROOT}/recompiler/src/pgxp_operation_flow.h"
            "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
            ${_generated} "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(_regions 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_CPU_SPANS AND PSX_OBSERVATION_TIME AND PSX_PGXP_DEMAND_REGIONS)
        if(PSX_PGXP_OPERATIONS)
            message(FATAL_ERROR "Demand regions and per-block precision operations are separate trials")
        endif()
        foreach(_source IN LISTS P_GENERATED_SOURCES)
            file(READ "${_source}" _text LIMIT 2048)
            if(NOT _text MATCHES "psx-pgxp-demand-regions-v2")
                message(FATAL_ERROR "Regenerate every game source before selecting PSX_PGXP_DEMAND_REGIONS: ${_source}")
            endif()
        endforeach()
        set(_regions 1)
    endif()
    target_compile_definitions(${target} PRIVATE PSX_PGXP_DEMAND_REGIONS=${_regions})
    file(CONFIGURE OUTPUT "${_dir}/demand.c" CONTENT
        "const unsigned psx_build_precision_demand_regions = ${_regions}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME precision-demand CONTRACT geometry-demand-recipes-${_regions}-v2
        HLE_SOURCES "${_dir}/demand.c" LLE_SOURCES "${_dir}/demand.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/pgxp_hooks.h"
            "${PSXRECOMP_ROOT}/runtime/include/psx_block_edge.h"
            "${PSXRECOMP_ROOT}/runtime/include/psx_native_span.h"
            "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
            "${PSXRECOMP_ROOT}/runtime/src/memory.c"
            "${PSXRECOMP_ROOT}/runtime/include/psx_direct_memory.h"
            "${PSXRECOMP_ROOT}/runtime/src/bios_hle.c"
            "${PSXRECOMP_ROOT}/runtime/include/bios_hle.h"
            "${PSXRECOMP_ROOT}/recompiler/src/pgxp_demand_regions.h"
            "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
            "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
