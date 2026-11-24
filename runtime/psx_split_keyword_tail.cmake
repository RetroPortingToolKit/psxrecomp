# psx_split_keyword_tail(<values_var> <tail_var> <tokens>...)
#
# psxrecomp_add_game_runtime() parses some list arguments itself
# (CODEGEN_SETUP_SOURCES, GEN_FULL_GLOB, EXCLUDE_BUILTIN_MODS, ...) and forwards
# keywords it does not know (APP_ICON, GAME_OVERLAY_STATIC_C, ...) to
# psxrecomp_add_runtime_target(). cmake_parse_arguments keeps collecting a
# multi-value list until it meets one of ITS OWN keywords, so a forwarded
# keyword written after one of those lists -- and its value -- would be
# swallowed as list items. List items are paths, globs, package ids and PCs,
# never a bare upper-case keyword, so everything from the first keyword-shaped
# token on is handed back as the tail to forward.
#
# Kept in its own file, with no project-mode commands, so that tests can drive
# it through `cmake -P`.
function(psx_split_keyword_tail values_var tail_var)
    set(_values "")
    set(_tail "")
    set(_in_tail FALSE)
    foreach(_tok IN LISTS ARGN)
        if(NOT _in_tail AND _tok MATCHES "^[A-Z][A-Z0-9_]*$")
            set(_in_tail TRUE)
        endif()
        if(_in_tail)
            list(APPEND _tail "${_tok}")
        else()
            list(APPEND _values "${_tok}")
        endif()
    endforeach()
    set(${values_var} "${_values}" PARENT_SCOPE)
    set(${tail_var} "${_tail}" PARENT_SCOPE)
endfunction()
