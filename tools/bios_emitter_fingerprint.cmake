# bios_emitter_fingerprint.cmake — THE emitter fingerprint, as a CMake script.
#
#   cmake -DPROFILE=bios/<stem>.toml [-DROOT=<framework>] [-DFP_DEBUG=1] \
#         [-DFP_LIST=1] -P tools/bios_emitter_fingerprint.cmake
#
# Prints one SHA-256 over every input that shapes the generated BIOS C: the
# sources of the psxrecomp-bios target, the local headers they include, the
# cycle headers they bake in, the profile and its seeds. Not the ROM image:
# the profile pins its SHA-256 and the emitter refuses any other image, and CI
# has no dump. FP_LIST=1 prints the inputs instead, one "<kind>\t<present|
# absent>\t<path>" line each (kind: source, header, extra, profile).
#
# The emitter files are read from add_executable(psxrecomp-bios ...) in
# recompiler/CMakeLists.txt, not kept by hand here: a hand list drifted from
# the target (it missed sources the target compiles and hashed files it does
# not). Local headers are followed through quoted #include lines, resolved
# against the including file's folder, recompiler/include and recompiler/src;
# files outside ROOT are skipped. A target this script cannot read (a variable
# or generator expression in its source list) is an error, not an empty list.
# Without recompiler/CMakeLists.txt (an install with built emitters) only the
# profile inputs are hashed.
#
# WHY CMAKE AND NOT BASH. runtime.cmake compares this against the committed
# generated/<stem>.emitter.sha at configure (fatal in release CI). A bash
# implementation hashed differently depending on which bash/coreutils CMake
# found on a Windows runner (Git Bash in the step shell agreed with Linux; the
# bash CMake located did not), so a byte-identical tree read as STALE there
# only. cmake is the one tool every configure already has, and
# file(READ)+string(SHA256) behave the same everywhere. Carriage returns are
# stripped before hashing so a CRLF checkout agrees with an LF one.
#
# Digest shape: per existing file, "<sha256 of CR-stripped bytes>  -\n", in
# the order sources, headers (sorted), cycle headers, profile, seeds; the
# fingerprint is the SHA-256 of that concatenated listing.
cmake_minimum_required(VERSION 3.20)
if(NOT ROOT)
    get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
# Native Windows separators would break the path comparisons below.
file(TO_CMAKE_PATH "${ROOT}" ROOT)
if(NOT PROFILE)
    set(PROFILE "bios/SCPH1001.toml")
endif()
if(NOT IS_ABSOLUTE "${PROFILE}")
    set(PROFILE "${ROOT}/${PROFILE}")
endif()

set(_paths)
set(_kinds)
macro(fp_add kind path)
    list(APPEND _paths "${path}")
    list(APPEND _kinds "${kind}")
endmacro()

if(EXISTS "${ROOT}/recompiler/CMakeLists.txt")
    file(READ "${ROOT}/recompiler/CMakeLists.txt" _cmake)
    string(REGEX REPLACE "#[^\n]*" "" _cmake "${_cmake}")
    string(REGEX MATCH "add_executable[ \t\r\n]*\\([ \t\r\n]*psxrecomp-bios[ \t\r\n]+([^)]*)\\)" _target "${_cmake}")
    if(NOT _target)
        message(FATAL_ERROR "bios_emitter_fingerprint: cannot read the psxrecomp-bios target")
    endif()
    set(_source_text "${CMAKE_MATCH_1}")
    if(_source_text MATCHES "[$<>]")
        message(FATAL_ERROR "bios_emitter_fingerprint: the psxrecomp-bios source list needs CMake expansion")
    endif()
    separate_arguments(_sources UNIX_COMMAND "${_source_text}")
    set(_queue)
    foreach(_src IN LISTS _sources)
        get_filename_component(_src "${_src}" ABSOLUTE BASE_DIR "${ROOT}/recompiler")
        if(NOT EXISTS "${_src}")
            message(FATAL_ERROR "bios_emitter_fingerprint: missing source ${_src}")
        endif()
        fp_add(source "${_src}")
        list(APPEND _queue "${_src}")
    endforeach()
    set(_seen "${_queue}")
    set(_headers)
    while(_queue)
        list(POP_FRONT _queue _file)
        get_filename_component(_dir "${_file}" DIRECTORY)
        file(STRINGS "${_file}" _includes REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
        foreach(_line IN LISTS _includes)
            string(REGEX MATCH "include[ \t]*\"([^\"]+)\"" _match "${_line}")
            set(_name "${CMAKE_MATCH_1}")
            foreach(_base IN ITEMS "${_dir}" "${ROOT}/recompiler/include" "${ROOT}/recompiler/src")
                get_filename_component(_inc "${_name}" ABSOLUTE BASE_DIR "${_base}")
                file(RELATIVE_PATH _rel "${ROOT}" "${_inc}")
                if(NOT _rel MATCHES "^\\.\\." AND EXISTS "${_inc}")
                    if(NOT _inc IN_LIST _seen)
                        list(APPEND _seen "${_inc}")
                        list(APPEND _headers "${_inc}")
                        list(APPEND _queue "${_inc}")
                    endif()
                    break()
                endif()
            endforeach()
        endforeach()
    endwhile()
    list(SORT _headers)
    foreach(_header IN LISTS _headers)
        fp_add(header "${_header}")
    endforeach()
    # Cycle headers the emitter bakes into the C it writes, unless an include
    # line already brought them in.
    foreach(_extra IN ITEMS "${ROOT}/runtime/include/psx_cyc.h" "${ROOT}/runtime/include/psx_instr_cost.h")
        if(NOT _extra IN_LIST _seen)
            fp_add(extra "${_extra}")
        endif()
    endforeach()
endif()

# Per-profile inputs: the profile itself and the seeds file it names.
if(EXISTS "${PROFILE}")
    fp_add(profile "${PROFILE}")
    file(STRINGS "${PROFILE}" _seed_lines REGEX "^[ \t]*seeds[ \t]*=[ \t]*\"[^\"]*\"")
    if(_seed_lines)
        list(GET _seed_lines 0 _seed_line)
        string(REGEX REPLACE "^[ \t]*seeds[ \t]*=[ \t]*\"([^\"]*)\".*$" "\\1" _seeds "${_seed_line}")
        if(_seeds)
            fp_add(profile "${ROOT}/${_seeds}")
        endif()
    endif()
else()
    fp_add(profile "${ROOT}/recompiler/seeds/phase2_ghidra_seeds.json")
endif()

set(_listing "")
set(_inventory "")
list(LENGTH _paths _count)
math(EXPR _last "${_count} - 1")
foreach(_i RANGE 0 ${_last})
    list(GET _paths ${_i} _p)
    list(GET _kinds ${_i} _kind)
    file(RELATIVE_PATH _rel "${ROOT}" "${_p}")
    if(NOT EXISTS "${_p}")
        string(APPEND _inventory "${_kind}\tabsent\t${_rel}\n")
        continue()
    endif()
    string(APPEND _inventory "${_kind}\tpresent\t${_rel}\n")
    file(READ "${_p}" _content)
    string(REPLACE "\r" "" _content "${_content}")
    string(SHA256 _d "${_content}")
    if(FP_DEBUG)
        message("${_d}  ${_rel}")
    endif()
    string(APPEND _listing "${_d}  -\n")
endforeach()
if(FP_LIST)
    string(STRIP "${_inventory}" _output)
else()
    string(SHA256 _output "${_listing}")
endif()
# message() writes to stderr; the digest (or list) is the script's stdout contract.
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_output}")
