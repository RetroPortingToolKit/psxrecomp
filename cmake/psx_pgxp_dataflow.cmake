include_guard(GLOBAL)
option(PSX_PGXP_DATAFLOW "Omit CFG-proven unused precision computations" OFF)

function(psxrecomp_pgxp_dataflow target)
    cmake_parse_arguments(P "" "" "GENERATED_SOURCES" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unexpected precision-dataflow arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    set(_generation_contract "")
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_CPU_SPANS AND PSX_PGXP_DATAFLOW)
        set(_native 1)
        foreach(_source IN LISTS P_GENERATED_SOURCES)
            get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(NOT EXISTS "${_source}")
                message(FATAL_ERROR "Precision dataflow requires regenerated game sources: ${_source}")
            endif()
            file(READ "${_source}" _text LIMIT 2048)
            if(NOT _text MATCHES "psx-pgxp-dataflow-v1")
                message(FATAL_ERROR "Regenerate game sources before selecting PSX_PGXP_DATAFLOW: ${_source}")
            endif()
        endforeach()
        set(_generation_contract ${P_GENERATED_SOURCES})
    endif()
    target_compile_definitions(${target} PRIVATE PSX_PGXP_DATAFLOW=${_native})
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/precision-dataflow/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/selected.c" CONTENT
        "const unsigned psx_build_precision_dataflow = ${_native}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME precision-dataflow CONTRACT cfg-dead-shadow-${_native}-v1
        HLE_SOURCES "${_directory}/selected.c"
        LLE_SOURCES "${_directory}/selected.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/recompiler/src/pgxp_dataflow.h"
                       "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
                       "${PSXRECOMP_ROOT}/runtime/include/pgxp_hooks.h"
                       "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
                       ${_generation_contract}
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
