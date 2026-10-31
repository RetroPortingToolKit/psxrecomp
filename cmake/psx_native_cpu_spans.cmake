include_guard(GLOBAL)
option(PSX_NATIVE_CPU_SPANS "Use generated native CPU span timing in ENHANCED builds" ON)

function(psxrecomp_native_cpu_spans target)
    if(NOT PSX_NATIVE_CPU_SPANS)
        return()
    endif()
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    if(_profile STREQUAL "ENHANCED")
        target_compile_definitions(${target} PRIVATE PSX_NATIVE_CPU_SPANS=1)
        foreach(_source IN LISTS ARGN)
            get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(NOT EXISTS "${_source}")
                message(FATAL_ERROR "Native CPU spans require materialized regenerated game sources: ${_source}")
            endif()
            file(READ "${_source}" _text LIMIT 1024)
            if(NOT _text MATCHES "psx_native_span[.]h")
                message(FATAL_ERROR "Regenerate game sources with native span support before selecting PSX_NATIVE_CPU_SPANS: ${_source}")
            endif()
        endforeach()
    endif()
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/native-cpu-spans/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/native.c" CONTENT
        "const unsigned psx_build_native_cpu_span_model = 1u;\n" @ONLY)
    file(CONFIGURE OUTPUT "${_directory}/reference.c" CONTENT
        "const unsigned psx_build_native_cpu_span_model = 0u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME cpu-spans CONTRACT generated-native-span-v1
        HLE_SOURCES "${_directory}/native.c"
        LLE_SOURCES "${_directory}/reference.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/psx_native_span.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_instr_cost.h"
                       "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/full_function_emitter.cpp")
endfunction()
