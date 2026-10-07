include_guard(GLOBAL)

function(psxrecomp_build_revision output source_dir)
    # A native CMake invocation can still inherit an MSYS Git first on PATH.
    # Prefer the Windows executable for Windows paths, retaining an explicit
    # PSX_BUILD_GIT override and the normal PATH search on other hosts.
    if(CMAKE_HOST_WIN32)
        find_program(PSX_BUILD_GIT NAMES git.exe
            PATHS "$ENV{ProgramFiles}/Git/cmd" "$ENV{ProgramW6432}/Git/cmd"
                  "C:/Program Files/Git/cmd"
            NO_DEFAULT_PATH)
    endif()
    find_program(PSX_BUILD_GIT NAMES git)
    set(_revision "unknown")
    if(PSX_BUILD_GIT)
        execute_process(
            COMMAND "${PSX_BUILD_GIT}" -C "${source_dir}"
                    describe --always --dirty --tags
            OUTPUT_VARIABLE _observed OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _result ERROR_QUIET TIMEOUT 20)
        if("${_result}" STREQUAL "0" AND NOT "${_observed}" STREQUAL "")
            set(_revision "${_observed}")
        endif()
    endif()
    set(${output} "${_revision}" PARENT_SCOPE)
endfunction()
