include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/psx_pgxp_deprecation.cmake")
option(PSX_NATIVE_PRECISION_SLICES "DEPRECATED: use qualified demand regions instead" OFF)
psxrecomp_reject_deprecated_pgxp_options()

function(psxrecomp_precision_slices target)
    psxrecomp_reject_deprecated_pgxp_options()
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_PRECISION_SLICES=${_native})
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/precision-slices/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/selected.c" CONTENT
        "const unsigned psx_build_precision_slice_model = ${_native}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME precision-slices CONTRACT plain-alu-${_native}-v1
        HLE_SOURCES "${_directory}/selected.c"
        LLE_SOURCES "${_directory}/selected.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/pgxp_hooks.h"
                       "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/pgxp_hook_emitter.h"
                       "${PSXRECOMP_ROOT}/recompiler/src/pgxp_hook_emitter.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
