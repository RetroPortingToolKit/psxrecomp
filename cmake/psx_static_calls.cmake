include_guard(GLOBAL)
option(PSX_NATIVE_STATIC_CALLS "Resolve qualified static transfers before generic dispatch" OFF)

function(psxrecomp_static_calls target game_dispatch)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_native 0)
    if(_profile STREQUAL "ENHANCED" AND PSX_NATIVE_STATIC_CALLS)
        set(_native 1)
        if(NOT EXISTS "${game_dispatch}")
            message(FATAL_ERROR "Native static transfers require a regenerated game dispatcher")
        endif()
        file(STRINGS "${game_dispatch}" _static_abi
             REGEX "#include .psx_static_calls.h." LIMIT_COUNT 1)
        if(NOT _static_abi)
            message(FATAL_ERROR "Regenerate game output before enabling PSX_NATIVE_STATIC_CALLS")
        endif()
        get_target_property(_sources ${target} SOURCES)
        foreach(_source IN LISTS _sources)
            if(_source MATCHES "_dispatch[.]c$" AND NOT _source STREQUAL "${game_dispatch}")
                if(NOT EXISTS "${_source}")
                    message(FATAL_ERROR "Native static transfers require regenerated BIOS output: ${_source}")
                endif()
                file(STRINGS "${_source}" _owner_abi REGEX "PSX_NATIVE_STATIC_OWNER_V1" LIMIT_COUNT 1)
                if(NOT _owner_abi)
                    message(FATAL_ERROR "Regenerate BIOS output before enabling PSX_NATIVE_STATIC_CALLS: ${_source}")
                endif()
            endif()
        endforeach()
    endif()
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_STATIC_CALLS=${_native})
    psxrecomp_add_implementation(${target}
        NAME static-native-calls CONTRACT write-validated-cps-${_native}-v1
        HLE_SOURCES "${PSXRECOMP_ROOT}/runtime/src/static_code_validity.c"
        LLE_SOURCES "${PSXRECOMP_ROOT}/runtime/src/static_code_validity.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/psx_static_calls.h"
                       "${PSXRECOMP_ROOT}/runtime/include/dirty_ram_interp.h"
                       "${PSXRECOMP_ROOT}/runtime/src/memory.c"
                       "${PSXRECOMP_ROOT}/runtime/src/dirty_ram_interp.c"
                       "${PSXRECOMP_ROOT}/recompiler/src/game_dispatch_emitter.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/full_function_emitter.cpp"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
