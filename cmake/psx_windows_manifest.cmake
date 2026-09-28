# Windows application manifest for every shipped psxrecomp executable.
#
# The manifest (assets/windows/psxrecomp.manifest) sets the process ANSI code
# page to UTF-8 on Windows 10 1903+, so narrow Win32 calls, the CRT and argv
# agree with the UTF-8 that SDL, tinyfiledialogs and our literals use
# (issue #371). Include this at file scope (it enables the RC language), then
# call psxrecomp_windows_manifest(<target>) on each executable. DLLs do not
# take one: the process code page comes from the exe's manifest alone.

include_guard(DIRECTORY)

set(PSXRECOMP_WINDOWS_MANIFEST
    "${CMAKE_CURRENT_LIST_DIR}/../assets/windows/psxrecomp.manifest")
get_filename_component(PSXRECOMP_WINDOWS_MANIFEST
    "${PSXRECOMP_WINDOWS_MANIFEST}" ABSOLUTE)

if(WIN32)
    # clang/llvm-mingw CI needs an explicit RC compiler hint; MSVC and MSYS2
    # GCC find theirs on their own.
    if(NOT MSVC AND NOT CMAKE_RC_COMPILER)
        find_program(CMAKE_RC_COMPILER
            NAMES llvm-rc llvm-windres windres
            HINTS
                "$ENV{RETCOMM_TOOLCHAIN}/bin"
                "$ENV{CMAKE_CLANG_V1}/bin"
            DOC "Windows resource compiler (manifest, app icon)")
    endif()
    enable_language(RC)
    if(NOT CMAKE_RC_COMPILER)
        message(FATAL_ERROR
            "psxrecomp: no Windows resource compiler (llvm-rc/windres/rc). "
            "It is required to embed the UTF-8 code-page manifest; without it "
            "non-ASCII paths and dialog text break on non-UTF-8 Windows locales.")
    endif()
endif()

function(psxrecomp_windows_manifest target)
    if(NOT WIN32)
        return()
    endif()
    if(MSVC)
        # A .manifest source is merged by the MSVC linker via /MANIFESTINPUT.
        target_sources(${target} PRIVATE "${PSXRECOMP_WINDOWS_MANIFEST}")
    else()
        # MinGW: RT_MANIFEST (24) resource with CREATEPROCESS_MANIFEST_RESOURCE_ID
        # (1). binutils and lld let it replace the toolchain's default-manifest.o.
        string(REPLACE "\\" "/" _manifest_fwd "${PSXRECOMP_WINDOWS_MANIFEST}")
        set(_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_manifest.rc")
        set(_content "1 24 \"${_manifest_fwd}\"\n")
        if(EXISTS "${_rc}")
            file(READ "${_rc}" _old)
        else()
            set(_old "")
        endif()
        if(NOT _old STREQUAL _content)
            file(WRITE "${_rc}" "${_content}")
        endif()
        target_sources(${target} PRIVATE "${_rc}")
        set_property(SOURCE "${_rc}" APPEND PROPERTY
            OBJECT_DEPENDS "${PSXRECOMP_WINDOWS_MANIFEST}")
    endif()
endfunction()
