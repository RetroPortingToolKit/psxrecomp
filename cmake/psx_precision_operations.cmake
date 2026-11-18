include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/psx_pgxp_deprecation.cmake")
option(PSX_PGXP_OPERATIONS "DEPRECATED: use qualified demand regions instead" OFF)
option(PSX_PGXP_DEMAND_REGIONS "Compile reconstructible integer precision at observation boundaries" OFF)
psxrecomp_reject_deprecated_pgxp_options()
function(psxrecomp_precision_operations target)
    psxrecomp_reject_deprecated_pgxp_options()
    cmake_parse_arguments(P "" "" "GENERATED_SOURCES" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unexpected precision-operation arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_selected 0)
    set(_generated "")
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
