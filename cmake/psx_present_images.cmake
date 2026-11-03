include_guard(GLOBAL)
option(PSX_SHARED_PRESENT_IMAGES "Share completed temporal images with hold-last" ON)

# Presentation-only ownership: this does not alter simulation or peer identity.
function(psxrecomp_present_images target)
    get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
    set(_selected 0)
    if(PSX_SHARED_PRESENT_IMAGES AND _profile STREQUAL "ENHANCED")
        set(_selected 1)
    endif()
    target_compile_definitions(${target} PRIVATE PSX_SHARED_PRESENT_IMAGES=${_selected})
    set_property(TARGET ${target} PROPERTY PSX_SHARED_PRESENT_IMAGES ${_selected})
endfunction()
