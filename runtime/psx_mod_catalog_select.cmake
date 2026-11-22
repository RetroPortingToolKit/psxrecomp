# Which framework builtin mod packages a runtime target stages.
#
# Every builtin under mods/builtin/packages targets game_id "*", so by default
# every title ships every one of them. A title may decline one it does not
# want -- because it ships its own replacement (WipEout 3's team-mark bezel
# supersedes the generic file-picker bezel) or because the feature is wrong for
# it -- by naming the package in EXCLUDE_BUILTIN_MODS on
# psxrecomp_add_runtime_target() / psxrecomp_add_game_runtime().
#
# An excluded package is ABSENT, not hidden: it is never copied into
# <exe-dir>/mods/bundled, so the launcher cannot list it, no release packager
# can ship it, and a stale state.toml naming it has nothing to reach.
#
# Kept in its own file, with no project-mode commands, so that
# runtime/tests/test_mod_catalog_layout.py can drive the real selection logic
# through `cmake -P` without configuring a whole runtime.
#
# psx_select_builtin_mod_ids(<out_var>
#     LABEL     <target name, for messages>
#     AVAILABLE <ids>...   # package directories under mods/builtin/packages
#     [ALLOWLIST <ids>...] # PSX_BUILTIN_MOD_ALLOWLIST, when set
#     [EXCLUDE   <ids>...] # the target's EXCLUDE_BUILTIN_MODS
#     [TITLE     <ids>...] # the target's own PRELOADED_MODS_DIR package ids
# )
#
# Fails configure loudly on:
#   * an excluded id that is not a framework builtin (a typo would otherwise
#     leave the package shipping while the CMakeLists reads as if it did not);
#   * an excluded id the title's own catalog also provides (a title catalog may
#     OVERRIDE a builtin at the same id -- excluding and overriding the same id
#     at once contradicts itself, and either reading would surprise someone).
# psx_split_exclude_builtin_mods(<ids_var> <tail_var> <tokens>...)
#
# psxrecomp_add_game_runtime() parses EXCLUDE_BUILTIN_MODS as a multi-value
# argument but forwards keywords it does not know (GAME_OVERLAY_STATIC_C,
# APP_ICON, ...) to psxrecomp_add_runtime_target(). cmake_parse_arguments keeps
# collecting a multi-value list until it meets one of ITS OWN keywords, so a
# forwarded keyword written after EXCLUDE_BUILTIN_MODS -- and its value -- would
# be swallowed as package ids. Package ids are lower-case dotted names, never
# an upper-case keyword, so everything from the first keyword-shaped token on
# is handed back as the tail to forward.
function(psx_split_exclude_builtin_mods ids_var tail_var)
    set(_ids "")
    set(_tail "")
    set(_in_tail FALSE)
    foreach(_tok IN LISTS ARGN)
        if(NOT _in_tail AND _tok MATCHES "^[A-Z][A-Z0-9_]*$")
            set(_in_tail TRUE)
        endif()
        if(_in_tail)
            list(APPEND _tail "${_tok}")
        else()
            list(APPEND _ids "${_tok}")
        endif()
    endforeach()
    set(${ids_var} "${_ids}" PARENT_SCOPE)
    set(${tail_var} "${_tail}" PARENT_SCOPE)
endfunction()

function(psx_select_builtin_mod_ids out_var)
    cmake_parse_arguments(_psxsel "" "LABEL" "AVAILABLE;ALLOWLIST;EXCLUDE;TITLE" ${ARGN})
    if(NOT _psxsel_LABEL)
        set(_psxsel_LABEL "runtime")
    endif()

    set(_selected ${_psxsel_AVAILABLE})
    if(_psxsel_ALLOWLIST)
        foreach(_id IN LISTS _psxsel_ALLOWLIST)
            if(NOT "${_id}" IN_LIST _psxsel_AVAILABLE)
                message(FATAL_ERROR
                    "PSX_BUILTIN_MOD_ALLOWLIST names missing package: ${_id}")
            endif()
        endforeach()
        set(_selected ${_psxsel_ALLOWLIST})
    endif()

    set(_unknown "")
    set(_overridden "")
    set(_exclude ${_psxsel_EXCLUDE})
    list(REMOVE_DUPLICATES _exclude)
    foreach(_id IN LISTS _exclude)
        if(NOT "${_id}" IN_LIST _psxsel_AVAILABLE)
            list(APPEND _unknown "${_id}")
        elseif("${_id}" IN_LIST _psxsel_TITLE)
            list(APPEND _overridden "${_id}")
        endif()
    endforeach()

    if(_unknown)
        list(JOIN _unknown "\n    " _pretty_unknown)
        list(JOIN _psxsel_AVAILABLE "\n    " _pretty_available)
        message(FATAL_ERROR
            "EXCLUDE_BUILTIN_MODS for target '${_psxsel_LABEL}' names package(s) "
            "that are not framework builtins:\n    ${_pretty_unknown}\n\n"
            "The framework's builtin packages (mods/builtin/packages) are:\n"
            "    ${_pretty_available}\n\n"
            "An unknown id is an error rather than a no-op: a misspelled exclusion "
            "would leave the package shipping while the CMakeLists reads as though "
            "it does not. See docs/MOD_PACKAGES.md.")
    endif()
    if(_overridden)
        list(JOIN _overridden "\n    " _pretty_overridden)
        message(FATAL_ERROR
            "EXCLUDE_BUILTIN_MODS for target '${_psxsel_LABEL}' excludes package(s) "
            "its own PRELOADED_MODS_DIR catalog also provides:\n"
            "    ${_pretty_overridden}\n\n"
            "A title catalog package with a builtin's id OVERRIDES that builtin, so "
            "excluding the same id contradicts it. Drop the id from "
            "EXCLUDE_BUILTIN_MODS to ship the title's override, or delete the "
            "title's package to ship neither.")
    endif()

    if(_exclude)
        list(REMOVE_ITEM _selected ${_exclude})
    endif()
    set(${out_var} "${_selected}" PARENT_SCOPE)
endfunction()
