include_guard(GLOBAL)
option(PSX_NATIVE_HOST_PACING "Share fine host deadline waits across presentation" ON)

# Host presentation only. The guest schedule and peer identity do not change.
function(psxrecomp_host_pacing target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_selected 0)
    if(PSX_NATIVE_HOST_PACING AND _profile STREQUAL "ENHANCED")
        set(_selected 1)
        target_sources(${target} PRIVATE "${PSXRECOMP_ROOT}/runtime/src/host_deadline.c")
    endif()
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_HOST_PACING=${_selected})
    set_property(TARGET ${target} PROPERTY PSX_NATIVE_HOST_PACING ${_selected})
endfunction()
