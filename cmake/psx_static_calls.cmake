include_guard(GLOBAL)
option(PSX_NATIVE_STATIC_CALLS "Resolve qualified static transfers before generic dispatch" OFF)
option(PSX_NATIVE_TAIL_CHAINS "Chain eligible CPS calls with compiler-guaranteed tail transfers" OFF)

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
    set(_tail 0)
    if(_native AND PSX_NATIVE_TAIL_CHAINS)
        # Failure selects the functioning original CPS path; an attribute
        # warning can never silently turn this into an ordinary recursive call.
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror=attributes")
        check_c_source_compiles([=[
#if !defined(__x86_64__) && !defined(__aarch64__)
#error unqualified tail-transfer ABI
#endif
#if defined(__clang__) && __has_attribute(musttail)
#define TAIL [[clang::musttail]]
#elif defined(__GNUC__) && __GNUC__ >= 15
#define TAIL __attribute__((musttail))
#else
#error no guaranteed tail-transfer statement
#endif
struct CPUState;
static void end(struct CPUState *p) { (void)p; }
static void (*volatile next)(struct CPUState *) = end;
static void chain(struct CPUState *p) { TAIL return next(p); }
int main(void) { chain(0); return 0; }
]=] PSX_COMPILER_GUARANTEED_TAIL_ABI)
        if(PSX_COMPILER_GUARANTEED_TAIL_ABI)
            file(STRINGS "${game_dispatch}" _tail_game REGEX "PSX_NATIVE_TAIL_GAME_V1" LIMIT_COUNT 1)
            if(NOT _tail_game)
                message(FATAL_ERROR "Regenerate game sources before enabling native tail chains")
            endif()
            foreach(_source IN LISTS _sources)
                if(_source MATCHES "_dispatch[.]c$" AND NOT _source STREQUAL "${game_dispatch}")
                    file(STRINGS "${_source}" _tail_owner REGEX "PSX_NATIVE_TAIL_OWNER_V1" LIMIT_COUNT 1)
                    if(NOT _tail_owner)
                        message(FATAL_ERROR "Regenerate BIOS owner before enabling native tail chains: ${_source}")
                    endif()
                endif()
            endforeach()
            set(_tail 1)
        else()
            message(STATUS "Guaranteed tail transfer unavailable: retaining portable flat CPS owner")
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_TAIL_CHAINS=${_tail})
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_STATIC_CALLS=${_native})
    psxrecomp_add_implementation(${target}
        NAME static-native-calls CONTRACT write-validated-cps-${_native}-tail-${_tail}-v2
        HLE_SOURCES "${PSXRECOMP_ROOT}/runtime/src/static_code_validity.c"
        LLE_SOURCES "${PSXRECOMP_ROOT}/runtime/src/static_code_validity.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/include/psx_static_calls.h"
                       "${PSXRECOMP_ROOT}/runtime/include/fntrace.h"
                       "${PSXRECOMP_ROOT}/runtime/src/fntrace.c"
                       "${PSXRECOMP_ROOT}/recompiler/src/code_generator.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/include/code_generator.h"
                       "${PSXRECOMP_ROOT}/runtime/include/dirty_ram_interp.h"
                       "${PSXRECOMP_ROOT}/runtime/src/memory.c"
                       "${PSXRECOMP_ROOT}/runtime/src/dirty_ram_interp.c"
                       "${PSXRECOMP_ROOT}/recompiler/src/game_dispatch_emitter.cpp"
                       "${PSXRECOMP_ROOT}/recompiler/src/full_function_emitter.cpp"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
