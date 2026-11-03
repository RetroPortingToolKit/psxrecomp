include_guard(GLOBAL)
option(PSX_NATIVE_RESIDENT_PREPARATION "Own verified resident blobs through cold preparation" ON)

function(psxrecomp_resident_preparation target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_selected 0)
    if(PSX_NATIVE_RESIDENT_PREPARATION AND _profile STREQUAL "ENHANCED")
        set(_selected 1)
    endif()
    target_compile_definitions(${target} PRIVATE PSX_NATIVE_RESIDENT_PREPARATION=${_selected})
    set_property(TARGET ${target} PROPERTY PSX_NATIVE_RESIDENT_PREPARATION_SELECTED ${_selected})
    # Host preparation ownership only: byte-identical content/key/metadata and
    # identical adapter callbacks are required. No simulation profile change.
endfunction()
