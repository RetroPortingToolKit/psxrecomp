include_guard(GLOBAL)
option(PSX_NATIVE_PRECISION_SLICES "Coalesce statically proven metadata-free ALU hooks" OFF)

function(psxrecomp_precision_slices target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_CPU_SPANS AND PSX_NATIVE_PRECISION_SLICES)
        set(_native 1)
    endif()
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
