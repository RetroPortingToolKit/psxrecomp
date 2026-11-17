include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/psx_pgxp_deprecation.cmake")
option(PSX_PGXP_DATAFLOW "DEPRECATED: use qualified demand regions instead" OFF)
psxrecomp_reject_deprecated_pgxp_options()

function(psxrecomp_pgxp_dataflow target)
    psxrecomp_reject_deprecated_pgxp_options()
    cmake_parse_arguments(P "" "" "GENERATED_SOURCES" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unexpected precision-dataflow arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    set(_generation_contract "")
    target_compile_definitions(${target} PRIVATE PSX_PGXP_DATAFLOW=${_native})
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/precision-dataflow/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/selected.c" CONTENT
        "const unsigned psx_build_precision_dataflow = ${_native}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME precision-dataflow CONTRACT cfg-shadow-lifetimes-${_native}-v2
        HLE_SOURCES "${_directory}/selected.c"
        LLE_SOURCES "${_directory}/selected.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/recompiler/src/pgxp_dataflow.h"
                       "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
                       "${PSXRECOMP_ROOT}/runtime/include/pgxp_hooks.h"
                       "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
                       ${_generation_contract}
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
