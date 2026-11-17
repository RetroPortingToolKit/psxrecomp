include_guard(GLOBAL)

# Recording is enabled by default in every release channel. The optional
# display remains elective, and mods/state.toml always owns explicit choices.
# Source/title packages provide regular visible Mods-page features.
function(psxrecomp_playtest_diagnostics_defaults target)
    cmake_parse_arguments(DIAG "" "MOD_ID" "" ${ARGN})
    if(DIAG_UNPARSED_ARGUMENTS OR NOT DIAG_MOD_ID MATCHES "^[a-z0-9._-]+$")
        message(FATAL_ERROR "psxrecomp_playtest_diagnostics_defaults requires MOD_ID")
    endif()
    if(NOT DEFINED PSX_PUBLIC_RELEASE)
        message(FATAL_ERROR "PSX_PUBLIC_RELEASE is undefined; include runtime/runtime.cmake first")
    endif()
    if(PSX_PUBLIC_RELEASE)
        set(_public 1)
    else()
        set(_public 0)
    endif()
    set(_targets ${target})
    if(TARGET ${target}-pgxp)
        list(APPEND _targets ${target}-pgxp)
    endif()
    foreach(_target IN LISTS _targets)
        set(_stage "${_target}_mod_catalog_stage")
        if(NOT TARGET ${_stage})
            message(FATAL_ERROR
                "playtest diagnostics defaults for ${_target}: ${_stage} does not "
                "exist. Declare the title catalog holding ${DIAG_MOD_ID} with "
                "PRELOADED_MODS_DIR so the framework stages it.")
        endif()
        get_target_property(_already ${_target} PSX_PLAYTEST_DIAGNOSTICS_MOD_ID)
        if(_already)
            message(FATAL_ERROR
                "playtest diagnostics defaults already configured for ${_target} (${_already})")
        endif()
        set_property(TARGET ${_target} PROPERTY PSX_PLAYTEST_DIAGNOSTICS_MOD_ID "${DIAG_MOD_ID}")
        # Runs inside the staging target, after it wipes and re-copies
        # mods/bundled, and before the catalog check and the game link.
        add_custom_command(TARGET ${_stage} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DPSX_MODS_DIR=$<TARGET_FILE_DIR:${_target}>/mods"
                "-DPSX_DIAGNOSTICS_ID=${DIAG_MOD_ID}"
                "-DPSX_PUBLIC_RELEASE=${_public}"
                "-DPSX_LABEL=${_target}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/psx_playtest_diagnostics_defaults.cmake"
            VERBATIM)
    endforeach()
    if(_public)
        message(STATUS "psxrecomp: ${target} public release; ${DIAG_MOD_ID} recording default on; optional display off")
    else()
        message(STATUS "psxrecomp: ${target} non-public build; ${DIAG_MOD_ID} recording default on; optional display off")
    endif()
endfunction()

# One implementation shared by every title; the mod manifest owns its features.
# Call after psxrecomp_add_game_runtime. This also covers its PGXP sibling.
function(psxrecomp_add_playtest_diagnostics target)
    cmake_parse_arguments(DIAG "NO_OSD" "GAME_ID;MOD_ID" "" ${ARGN})
    if(DIAG_UNPARSED_ARGUMENTS OR NOT DIAG_GAME_ID MATCHES "^[A-Za-z0-9_-]+$"
       OR NOT DIAG_MOD_ID MATCHES "^[a-z0-9._-]+$")
        message(FATAL_ERROR "playtest diagnostics requires GAME_ID and MOD_ID")
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(_version "development")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _version LIMIT_COUNT 1)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    endif()
    include("${_root}/cmake/psx_build_revision.cmake")
    foreach(_component game framework)
        set(_revision_${_component} "nogit")
        if(_component STREQUAL "game")
            set(_directory "${CMAKE_CURRENT_SOURCE_DIR}")
        else()
            set(_directory "${_root}")
        endif()
        psxrecomp_build_revision(_revision_${_component} "${_directory}")
    endforeach()
    set(_identity "${DIAG_GAME_ID}/${_version}+${_revision_game}/psxrecomp-${_revision_framework}")
    string(REGEX REPLACE "[^A-Za-z0-9._/+:-]" "_" _identity "${_identity}")
    # Restrict changing build identities to the recorder translation unit.
    # Generated game units can be enormous and do not use these definitions.
    set_property(SOURCE "${_root}/runtime/src/playtest_diagnostics.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS
        "PSX_DIAGNOSTIC_GAME_ID=\"$<TARGET_PROPERTY:PSX_PLAYTEST_DIAGNOSTIC_GAME_ID>\""
        "PSX_DIAGNOSTIC_MOD_ID=\"$<TARGET_PROPERTY:PSX_PLAYTEST_DIAGNOSTIC_MOD_ID>\""
        "PSX_DIAGNOSTIC_BUILD_ID=\"$<TARGET_PROPERTY:PSX_PLAYTEST_DIAGNOSTIC_BUILD_ID>\""
        "PSX_DIAGNOSTIC_WITH_OSD=$<TARGET_PROPERTY:PSX_PLAYTEST_DIAGNOSTIC_WITH_OSD>")
    set(_targets ${target})
    if(TARGET ${target}-pgxp)
        list(APPEND _targets ${target}-pgxp)
    endif()
    find_package(Threads REQUIRED)
    foreach(_target IN LISTS _targets)
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "playtest diagnostics target ${_target} does not exist")
        endif()
        get_target_property(_already ${_target} PSX_PLAYTEST_DIAGNOSTICS)
        if(_already)
            message(FATAL_ERROR "playtest diagnostics already configured for ${_target}")
        endif()
        set_property(TARGET ${_target} PROPERTY PSX_PLAYTEST_DIAGNOSTICS TRUE)
        # The same recorder source can serve multiple regional targets in this
        # directory. Its definitions resolve against the compiling target.
        set_target_properties(${_target} PROPERTIES
            PSX_PLAYTEST_DIAGNOSTIC_GAME_ID "${DIAG_GAME_ID}"
            PSX_PLAYTEST_DIAGNOSTIC_MOD_ID "${DIAG_MOD_ID}"
            PSX_PLAYTEST_DIAGNOSTIC_BUILD_ID "${_identity}")
        if(DIAG_NO_OSD)
            set_property(TARGET ${_target} PROPERTY PSX_PLAYTEST_DIAGNOSTIC_WITH_OSD 0)
        else()
            set_property(TARGET ${_target} PROPERTY PSX_PLAYTEST_DIAGNOSTIC_WITH_OSD 1)
        endif()
        target_sources(${_target} PRIVATE "${_root}/runtime/src/playtest_diagnostics.cpp")
        target_include_directories(${_target} PRIVATE "${_root}/runtime/include")
        target_compile_features(${_target} PRIVATE cxx_std_17)
        target_link_libraries(${_target} PRIVATE Threads::Threads)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_root}/tools/replay_diagnostics.py"
                "$<TARGET_FILE_DIR:${_target}>/replay-diagnostics.py" VERBATIM)
    endforeach()
    psxrecomp_playtest_diagnostics_defaults(${target} MOD_ID "${DIAG_MOD_ID}")
endfunction()
