# overlay_static_sources.cmake — resolve a title's build-time static overlay
# shard (GAME_OVERLAY_STATIC_C) into sources, loudly.
#
# The shard is generated from the player's own disc (psxrecomp_cli.py generate
# runs tools/aot_overlay_pipeline.py `static`), so it is legitimately absent in
# CI and on a fresh checkout. It used to be *silently* absent: the build simply
# linked no static overlays and every image it would cover ran on the dirty-RAM
# interpreter, with nothing to say so. This module keeps "absent" legal but
# visible, and refuses the one configuration that can never work — a profile
# that tells Generate to write one file while the build links another.
#
#   psxrecomp_overlay_static_sources(<out_sources_var> <out_present_var>
#       TARGET      <name>                 # for messages
#       STATIC_C    <path or empty>        # GAME_OVERLAY_STATIC_C
#       PROFILE     <aot/overlays.json>    # may not exist
#       GAME_LINKED <bool>)                # generated game C is linked
#
# <out_sources_var> receives the dispatcher and its _NNNN.c units when the
# dispatcher exists; <out_present_var> is TRUE when it does.

function(psxrecomp_overlay_static_sources out_sources out_present)
    cmake_parse_arguments(PSXOS "" "TARGET;STATIC_C;PROFILE;GAME_LINKED" "" ${ARGN})
    set(_sources "")
    set(_present FALSE)

    # ---- the profile's declaration must name the same file ---------------
    if(PSXOS_PROFILE AND EXISTS "${PSXOS_PROFILE}")
        file(READ "${PSXOS_PROFILE}" _profile_json)
        string(JSON _decl ERROR_VARIABLE _json_err GET "${_profile_json}" static_output)
        if(_json_err AND NOT _json_err MATCHES "not found")
            message(FATAL_ERROR
                "${PSXOS_TARGET}: cannot read static_output from ${PSXOS_PROFILE}: "
                "${_json_err}")
        endif()
        if(NOT _json_err AND NOT "${_decl}" STREQUAL "")
            # static_output is relative to the project root, the directory that
            # holds aot/ (and game.toml).
            get_filename_component(_profile_dir "${PSXOS_PROFILE}" DIRECTORY)
            get_filename_component(_project_root "${_profile_dir}" DIRECTORY)
            get_filename_component(_declared "${_decl}" ABSOLUTE BASE_DIR "${_project_root}")
            if("${PSXOS_STATIC_C}" STREQUAL "")
                message(FATAL_ERROR
                    "${PSXOS_TARGET}: ${PSXOS_PROFILE} declares static_output "
                    "\"${_decl}\", so Generate writes ${_declared}, but this target "
                    "passes no GAME_OVERLAY_STATIC_C and would never link it. Add\n"
                    "  GAME_OVERLAY_STATIC_C \"\${CMAKE_CURRENT_SOURCE_DIR}/${_decl}\"\n"
                    "to psxrecomp_add_game_runtime(...), or remove static_output from "
                    "the profile if this title stages DLL caches instead.")
            endif()
            get_filename_component(_linked "${PSXOS_STATIC_C}" ABSOLUTE)
            file(TO_CMAKE_PATH "${_declared}" _declared)
            file(TO_CMAKE_PATH "${_linked}" _linked)
            if(CMAKE_HOST_WIN32)
                string(TOLOWER "${_declared}" _declared_cmp)
                string(TOLOWER "${_linked}" _linked_cmp)
            else()
                set(_declared_cmp "${_declared}")
                set(_linked_cmp "${_linked}")
            endif()
            if(NOT _declared_cmp STREQUAL _linked_cmp)
                message(FATAL_ERROR
                    "${PSXOS_TARGET}: GAME_OVERLAY_STATIC_C is ${_linked}, but "
                    "${PSXOS_PROFILE} declares static_output \"${_decl}\" "
                    "(${_declared}). Generate writes the profile's path; the build "
                    "would link a file nothing produces. Make them name the same file.")
            endif()
        endif()
    endif()

    if("${PSXOS_STATIC_C}" STREQUAL "")
        set(${out_sources} "" PARENT_SCOPE)
        set(${out_present} FALSE PARENT_SCOPE)
        return()
    endif()

    # compile_overlays.py --static splits its output: overlays_static.c is the
    # dispatcher and each overlay is its own translation unit,
    # overlays_static_NNNN.c, beside it. The dispatcher is globbed too, with
    # CONFIGURE_DEPENDS, so a Generate that creates (or removes) it after this
    # configure re-runs configure at build time instead of leaving the binary
    # without it.
    get_filename_component(_dir  "${PSXOS_STATIC_C}" DIRECTORY)
    get_filename_component(_stem "${PSXOS_STATIC_C}" NAME_WE)
    get_filename_component(_name "${PSXOS_STATIC_C}" NAME)
    file(GLOB _main CONFIGURE_DEPENDS "${_dir}/${_name}")
    file(GLOB _parts CONFIGURE_DEPENDS "${_dir}/${_stem}_[0-9][0-9][0-9][0-9].c")
    if(_main)
        list(SORT _parts)
        set(_sources "${PSXOS_STATIC_C}" ${_parts})
        set(_present TRUE)
    elseif(PSXOS_GAME_LINKED)
        message(WARNING
            "${PSXOS_TARGET}: GAME_OVERLAY_STATIC_C is declared but "
            "${PSXOS_STATIC_C} does not exist, so this build links NO static "
            "overlays: every image it would cover runs on the dirty-RAM "
            "interpreter instead of as native code. It is generated from your "
            "own disc -- run\n"
            "  python psxrecomp/psxrecomp_cli.py generate --config game.toml "
            "--project-root . --disc <cue>\n"
            "(or tools/aot_overlay_pipeline.py static; docs/AOT_SHARDING.md).")
    endif()

    set(${out_sources} "${_sources}" PARENT_SCOPE)
    set(${out_present} ${_present} PARENT_SCOPE)
endfunction()
