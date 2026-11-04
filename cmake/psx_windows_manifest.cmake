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

# GNU drivers can append default-manifest.o independently of our RC object.
# Recent binutils retains both ID-1 entries and still exits successfully. Scope
# the endfile override to executables carrying our manifest, preserving every
# other CRT/endfile token and leaving Clang/MSVC untouched.
function(_psxrecomp_manifest_gnu_specs lang out)
    set(${out} "" PARENT_SCOPE)
    if(NOT CMAKE_${lang}_COMPILER_ID STREQUAL "GNU")
        return()
    endif()
    execute_process(COMMAND "${CMAKE_${lang}_COMPILER}" -dumpspecs
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _specs ERROR_QUIET)
    if(NOT _rc EQUAL 0 OR NOT _specs MATCHES "\n\\*endfile:\n([^\n]+)")
        return()
    endif()
    set(_endfile "${CMAKE_MATCH_1}")
    set(_injection "%:if-exists(default-manifest.o%s)")
    string(FIND "${_endfile}" "${_injection}" _found)
    if(_found EQUAL -1)
        return()
    endif()
    execute_process(COMMAND "${CMAKE_${lang}_COMPILER}" -print-file-name=default-manifest.o
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _default OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_default}")
        return()
    endif()
    string(REPLACE "${_injection}" "" _endfile "${_endfile}")
    set(_content "*endfile:\n${_endfile}\n")
    set(_path "${CMAKE_BINARY_DIR}/psx_manifest_${lang}.specs")
    set(_old "")
    if(EXISTS "${_path}")
        file(READ "${_path}" _old)
    endif()
    if(NOT _old STREQUAL _content)
        file(WRITE "${_path}" "${_content}")
    endif()
    set(${out} "${_path}" PARENT_SCOPE)
endfunction()

function(psxrecomp_windows_manifest target)
    if(NOT WIN32)
        return()
    endif()
    if(MSVC)
        # A .manifest source is merged by the MSVC linker via /MANIFESTINPUT.
        target_sources(${target} PRIVATE "${PSXRECOMP_WINDOWS_MANIFEST}")
    else()
        # MinGW: RT_MANIFEST (24) resource with CREATEPROCESS_MANIFEST_RESOURCE_ID
        # (1). GNU drivers need their injected default suppressed explicitly.
        foreach(_lang IN ITEMS C CXX)
            _psxrecomp_manifest_gnu_specs(${_lang} _specs)
            if(_specs)
                target_link_options(${target} PRIVATE
                    "$<$<LINK_LANG_AND_ID:${_lang},GNU>:-specs=${_specs}>")
                set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_specs}")
            endif()
        endforeach()
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
        # Isolate RC from executable definitions: windres cannot preserve
        # quoted pipe-separated BIOS names through its shell preprocessor.
        add_library(${target}_manifest OBJECT "${_rc}")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:${target}_manifest>)
        set_property(SOURCE "${_rc}" APPEND PROPERTY
            OBJECT_DEPENDS "${PSXRECOMP_WINDOWS_MANIFEST}")
    endif()
endfunction()
