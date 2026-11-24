# psx_partition_game_runtime_args(<wrapper_out> <runtime_out>
#                                 <options> <single_keys> <list_keys> <tokens>...)
#
# Split the original call before either layer uses cmake_parse_arguments.
# A bare uppercase keyword ends a wrapper list of paths, globs, package ids
# or PCs. Arguments for the runtime keep their order so repeated scalar
# options retain CMake's last-value precedence.
#
# Kept in its own file, with no project-mode commands, so that tests can drive
# it through `cmake -P`.
function(psx_partition_game_runtime_args wrapper_out runtime_out options single_keys list_keys)
    set(_wrapper_keys ${options} ${single_keys} ${list_keys})
    set(_wrapper_args "")
    set(_runtime_args "")
    set(_collect "")
    foreach(_token IN LISTS ARGN)
        if(_token IN_LIST _wrapper_keys)
            list(APPEND _wrapper_args "${_token}")
            set(_collect "${_token}")
        elseif(NOT _collect STREQUAL "" AND _collect IN_LIST single_keys)
            list(APPEND _wrapper_args "${_token}")
            set(_collect "")
        elseif(NOT _collect STREQUAL "" AND _collect IN_LIST list_keys AND
               NOT _token MATCHES "^[A-Z][A-Z0-9_]*$")
            list(APPEND _wrapper_args "${_token}")
        else()
            list(APPEND _runtime_args "${_token}")
            set(_collect "")
        endif()
    endforeach()
    set(${wrapper_out} "${_wrapper_args}" PARENT_SCOPE)
    set(${runtime_out} "${_runtime_args}" PARENT_SCOPE)
endfunction()
