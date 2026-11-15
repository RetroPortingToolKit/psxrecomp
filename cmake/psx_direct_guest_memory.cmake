include_guard(GLOBAL)
option(PSX_DIRECT_GUEST_MEMORY "Inline ENHANCED scratchpad reads and unobserved CPU stores" OFF)

function(psxrecomp_direct_guest_memory target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_CPU_SPANS AND PSX_DIRECT_GUEST_MEMORY)
        set(_native 1)
        foreach(_source IN LISTS ARGN)
            get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(NOT EXISTS "${_source}")
                message(FATAL_ERROR "Direct guest memory requires regenerated game sources: ${_source}")
            endif()
            file(READ "${_source}" _text LIMIT 2048)
            if(NOT _text MATCHES "psx-direct-memory-v1")
                message(FATAL_ERROR "Regenerate game sources before selecting PSX_DIRECT_GUEST_MEMORY: ${_source}")
            endif()
        endforeach()
    endif()
    target_compile_definitions(${target} PRIVATE PSX_DIRECT_GUEST_MEMORY=${_native})
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/direct-guest-memory/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/selected.c" CONTENT
        "const unsigned psx_build_direct_guest_memory_model = ${_native}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME direct-guest-memory CONTRACT native-data-pages-${_native}-v1
        HLE_SOURCES "${_directory}/selected.c" LLE_SOURCES "${_directory}/selected.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/psx_direct_memory.h"
                       "${PSXRECOMP_ROOT}/runtime/include/psx_native_span.h"
                       "${PSXRECOMP_ROOT}/runtime/include/cpu_state.h"
                       "${PSXRECOMP_ROOT}/runtime/src/memory.c"
                       "${PSXRECOMP_ROOT}/runtime/src/card_data_writes.c"
                       "${PSXRECOMP_ROOT}/runtime/src/parity_trace.c"
                       "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/main_psx.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/strict_translator.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/full_function_emitter.cpp"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
