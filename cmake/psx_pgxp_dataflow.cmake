include_guard(GLOBAL)
option(PSX_PGXP_DATAFLOW "Omit CFG-proven unused precision computations" OFF)

function(psxrecomp_pgxp_dataflow target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_CPU_SPANS AND PSX_PGXP_DATAFLOW)
        set(_native 1)
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
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
