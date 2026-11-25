include_guard(GLOBAL)
option(PSX_NATIVE_KERNEL "Use the strict host-native BIOS executive" ON)

function(psxrecomp_native_kernel target)
    set(_native 0)
    if(PSX_NATIVE_KERNEL)
        get_target_property(_profile ${target} PSX_EXECUTION_PROFILE)
        if(NOT _profile STREQUAL "ENHANCED")
            message(FATAL_ERROR "The native BIOS executive requires ENHANCED; select the reference backend explicitly for accuracy builds")
        endif()
        set(_native 1)
    endif()
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/native-kernel/${target}")
    file(MAKE_DIRECTORY "${_directory}")
    file(CONFIGURE OUTPUT "${_directory}/selected.c" CONTENT
        "const unsigned psx_build_native_kernel = ${_native}u;\n" @ONLY)
    psxrecomp_add_implementation(${target}
        NAME bios-kernel CONTRACT native-executive-${_native}-v1
        HLE_SOURCES "${_directory}/selected.c" LLE_SOURCES "${_directory}/selected.c"
        CONTRACT_FILES "${PSXRECOMP_ROOT}/runtime/src/bios_hle.c"
                       "${PSXRECOMP_ROOT}/runtime/src/native_bios_boot.cpp"
                       "${PSXRECOMP_ROOT}/runtime/src/native_bios_services.c"
                       "${PSXRECOMP_ROOT}/runtime/src/native_bios_card.c"
                       "${PSXRECOMP_ROOT}/runtime/include/bios_hle.h"
                       "${PSXRECOMP_ROOT}/runtime/include/native_bios_boot.h"
                       "${PSXRECOMP_ROOT}/runtime/include/native_bios_services.h"
                       "${PSXRECOMP_ROOT}/runtime/src/main.cpp"
                       "${PSXRECOMP_ROOT}/runtime/src/traps.c"
                       "${PSXRECOMP_ROOT}/runtime/src/interrupts.c"
                       "${PSXRECOMP_ROOT}/runtime/src/sio.c"
                       "${PSXRECOMP_ROOT}/runtime/src/disc_boot_image.cpp"
                       "${PSXRECOMP_ROOT}/runtime/src/iso_reader.cpp"
                       "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
endfunction()
