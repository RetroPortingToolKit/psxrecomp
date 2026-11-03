# Build-time step over one STAGED Playtest Diagnostics package.
#
# Owner policy (2026-10-19): Playtest Diagnostics (input recording + OSD) ship
# OFF by default only in public production releases. Every other build (dev
# trees, playtest/pre-release packages) defaults them ON. The feature stays a
# player-selectable mod either way.
#
# Source manifests carry the production default (false). This script runs
# right after the framework stages the mod catalog into <exe-dir>/mods/bundled
# and edits only that staged copy:
#
#   PSX_PUBLIC_RELEASE=0  set default_enabled = true for features recording/osd
#   PSX_PUBLIC_RELEASE=1  require they are explicitly default_enabled = false
#
# Inputs (-D):
#   PSX_MODS_DIR         <exe-dir>/mods
#   PSX_DIAGNOSTICS_ID   package id, e.g. jerseydevil.diagnostics
#   PSX_PUBLIC_RELEASE   0 or 1
#   PSX_LABEL            target name for messages (optional)
#
# Line-oriented on purpose: the manifests are flat [[table]] TOML, and a
# string-level rewrite keeps every other byte (comments, ordering) intact.
cmake_minimum_required(VERSION 3.20)

foreach(_required PSX_MODS_DIR PSX_DIAGNOSTICS_ID PSX_PUBLIC_RELEASE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "psx_playtest_diagnostics_defaults: ${_required} is required")
    endif()
endforeach()
if(NOT DEFINED PSX_LABEL)
    set(PSX_LABEL "${PSX_DIAGNOSTICS_ID}")
endif()
if(PSX_PUBLIC_RELEASE)
    set(_public TRUE)
    set(_mode "public release")
else()
    set(_public FALSE)
    set(_mode "non-public build")
endif()

file(GLOB _manifests "${PSX_MODS_DIR}/bundled/${PSX_DIAGNOSTICS_ID}/*/manifest.toml")
if(NOT _manifests)
    message(FATAL_ERROR
        "[${PSX_LABEL}] Playtest Diagnostics package ${PSX_DIAGNOSTICS_ID} was not "
        "staged: no ${PSX_MODS_DIR}/bundled/${PSX_DIAGNOSTICS_ID}/<version>/manifest.toml. "
        "Author it under the title's mods/preloaded/packages (docs/PLAYTEST_DIAGNOSTICS.md).")
endif()

# Rewrites/validates one [[feature]] block. Sets _blk and appends the feature
# id to _seen in the caller's scope when the block is one of ours.
function(_psx_diag_block blk out_blk seen_var)
    set(_seen ${${seen_var}})
    string(REGEX MATCH "^[ \t]*\\[\\[feature\\]\\]" _is_feature "${blk}")
    if(NOT _is_feature)
        set(${out_blk} "${blk}" PARENT_SCOPE)
        return()
    endif()
    set(_id "")
    if(blk MATCHES "\n[ \t]*id[ \t]*=[ \t]*\"([^\"]*)\"")
        set(_id "${CMAKE_MATCH_1}")
    endif()
    if(NOT _id STREQUAL "recording" AND NOT _id STREQUAL "osd")
        set(${out_blk} "${blk}" PARENT_SCOPE)
        return()
    endif()
    list(APPEND _seen "${_id}")
    set(_def_match FALSE)
    set(_value "<missing>")
    if(blk MATCHES "\n[ \t]*default_enabled[ \t]*=[ \t]*([A-Za-z]+)")
        set(_def_match TRUE)
        set(_value "${CMAKE_MATCH_1}")
    endif()
    if(_public)
        if(NOT _value STREQUAL "false")
            message(FATAL_ERROR
                "[${PSX_LABEL}] ${_manifest}: feature '${_id}' must say "
                "default_enabled = false in the source manifest; public releases "
                "ship Playtest Diagnostics off by default (found '${_value}').")
        endif()
    elseif(_def_match)
        string(REGEX REPLACE "\n([ \t]*)default_enabled[ \t]*=[ \t]*[A-Za-z]+"
               "\n\\1default_enabled = true" blk "${blk}")
    else()
        # No explicit default: insert one directly under the table header.
        string(FIND "${blk}" "\n" _nl)
        math(EXPR _after "${_nl} + 1")
        string(SUBSTRING "${blk}" 0 ${_after} _head)
        string(SUBSTRING "${blk}" ${_after} -1 _tail)
        set(blk "${_head}default_enabled = true\n${_tail}")
    endif()
    set(${out_blk} "${blk}" PARENT_SCOPE)
    set(${seen_var} "${_seen}" PARENT_SCOPE)
endfunction()

foreach(_manifest IN LISTS _manifests)
    file(READ "${_manifest}" _text)
    set(_out "")
    set(_blk "")
    set(_seen "")
    # Split into [table] blocks; text before the first header is its own block.
    while(NOT _text STREQUAL "")
        string(FIND "${_text}" "\n" _nl)
        if(_nl EQUAL -1)
            set(_line "${_text}")
            set(_text "")
        else()
            math(EXPR _n "${_nl} + 1")
            string(SUBSTRING "${_text}" 0 ${_n} _line)
            string(SUBSTRING "${_text}" ${_n} -1 _text)
        endif()
        if(_line MATCHES "^[ \t]*\\[")
            _psx_diag_block("${_blk}" _blk _seen)
            string(APPEND _out "${_blk}")
            set(_blk "${_line}")
        else()
            string(APPEND _blk "${_line}")
        endif()
    endwhile()
    _psx_diag_block("${_blk}" _blk _seen)
    string(APPEND _out "${_blk}")

    foreach(_feature recording osd)
        list(FIND _seen "${_feature}" _found)
        if(_found EQUAL -1)
            message(FATAL_ERROR
                "[${PSX_LABEL}] ${_manifest} has no [[feature]] id = \"${_feature}\"; "
                "Playtest Diagnostics needs both recording and osd features.")
        endif()
    endforeach()

    file(READ "${_manifest}" _before)
    if(NOT _out STREQUAL _before)
        # file(WRITE) translates LF to CRLF on some Windows CMake builds
        # (measured: MSYS2 mingw64 cmake 4.2.2). file(CONFIGURE) takes an
        # explicit newline style, so the staged bytes keep the source's.
        # @ONLY: escape '@' so no @VAR@ substitution can occur.
        if(_out MATCHES "#cmakedefine")
            message(FATAL_ERROR "[${PSX_LABEL}] ${_manifest}: unsupported '#cmakedefine' text")
        endif()
        # The same builds also drop CR on file(READ); detect CRLF from bytes.
        file(READ "${_manifest}" _hex HEX)
        if(_hex MATCHES "^(..)*0d0a")
            set(_newline CRLF)
        else()
            set(_newline LF)
        endif()
        string(REPLACE "\r\n" "\n" _out "${_out}")
        set(_psx_at "@")
        string(REPLACE "@" "@_psx_at@" _out "${_out}")
        file(CONFIGURE OUTPUT "${_manifest}" CONTENT "${_out}" @ONLY NEWLINE_STYLE ${_newline})
    endif()
    if(_public)
        message(STATUS "[${PSX_LABEL}] ${PSX_DIAGNOSTICS_ID}: ${_mode}, recording/osd default_enabled = false")
    else()
        message(STATUS "[${PSX_LABEL}] ${PSX_DIAGNOSTICS_ID}: ${_mode}, recording/osd default_enabled = true")
    endif()
endforeach()
