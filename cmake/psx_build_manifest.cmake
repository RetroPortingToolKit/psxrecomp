# Separate from semantic execution profiles: conservative declared build inputs.
# This is not an executable hash or permission to mix different builds.
include_guard(GLOBAL)

function(_psx_build_manifest_finalize target)
    get_target_property(_python ${target} PSX_BUILD_MANIFEST_PYTHON)
    get_target_property(_script ${target} PSX_BUILD_MANIFEST_SCRIPT)
    get_target_property(_source ${target} PSX_BUILD_MANIFEST_SOURCE_DIR)
    get_target_property(_binary ${target} PSX_BUILD_MANIFEST_BINARY_DIR)
    get_target_property(_roots ${target} PSX_BUILD_MANIFEST_ROOTS)
    get_target_property(_headers ${target} PSX_BUILD_MANIFEST_HEADERS)
    get_target_property(_inputs ${target} PSX_BUILD_MANIFEST_INPUTS)
    get_target_property(_identities ${target} PSX_BUILD_MANIFEST_DEPENDENCY_IDENTITIES)
    # ASCII US separates arbitrary multiline values without shell/JSON escaping.
    string(ASCII 31 _sep)
    set(_spec "schema${_sep}1${_sep}source_dir${_sep}${_source}${_sep}roots${_sep}${_roots}${_sep}headers${_sep}${_headers}${_sep}inputs${_sep}${_inputs}${_sep}dependency_identities${_sep}${_identities}${_sep}configuration${_sep}$<CONFIG>")
    foreach(_var CMAKE_C_COMPILER_ID CMAKE_C_COMPILER_VERSION CMAKE_C_COMPILER_TARGET
            CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION CMAKE_CXX_COMPILER_TARGET
            CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR CMAKE_SIZEOF_VOID_P
            CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS)
        string(APPEND _spec "${_sep}${_var}${_sep}${${_var}}")
        foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            if(DEFINED ${_var}_${_config})
                string(APPEND _spec "${_sep}${_var}_${_config}${_sep}${${_var}_${_config}}")
            endif()
        endforeach()
    endforeach()
    foreach(_property COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES
            COMPILE_FEATURES LINK_OPTIONS LINK_LIBRARIES C_STANDARD CXX_STANDARD
            INTERPROCEDURAL_OPTIMIZATION)
        string(APPEND _spec "${_sep}target.${_property}${_sep}$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},${_property}>>")
    endforeach()
    get_target_property(_sources ${target} SOURCES)
    set(_identity_sources "")
    set(_object_dependencies "")
    foreach(_file IN LISTS _sources)
        set(_identity_file "${_file}")
        if(_file MATCHES "^\\$<TARGET_OBJECTS:([^>]+)>$")
            list(APPEND _object_dependencies "${CMAKE_MATCH_1}")
        endif()
        if(_file MATCHES "\\$<")
            list(APPEND _identity_sources "${_file}")
            continue() # Resolved source bytes are still captured above.
        endif()
        get_source_file_property(_original "${_file}" PSX_BUILD_MANIFEST_ORIGINAL_SOURCE)
        if(_original AND NOT _original STREQUAL "NOTFOUND")
            set(_identity_file "${_original}")
        endif()
        list(APPEND _identity_sources "${_identity_file}")
        foreach(_property LANGUAGE COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FLAGS INCLUDE_DIRECTORIES)
            get_source_file_property(_value "${_file}" ${_property})
            if(NOT _value STREQUAL "NOTFOUND")
                string(APPEND _spec "${_sep}source.${_identity_file}.${_property}${_sep}$<TARGET_GENEX_EVAL:${target},${_value}>")
            endif()
        endforeach()
    endforeach()
    string(APPEND _spec "${_sep}sources${_sep}$<TARGET_GENEX_EVAL:${target},${_identity_sources}>")
    set(_dir "${_binary}/psx_build_manifest/${target}/$<CONFIG>")
    # A removed compile language must not leave an old specification eligible
    # for the next build. These are only this target's generated input records.
    file(GLOB _old_specs "${_binary}/psx_build_manifest/${target}/*/inputs-*.txt")
    if(_old_specs)
        file(REMOVE ${_old_specs})
    endif()
    string(APPEND _spec "${_sep}compile_language${_sep}$<COMPILE_LANGUAGE>")
    file(GENERATE OUTPUT "${_dir}/inputs-$<COMPILE_LANGUAGE>.txt" CONTENT "${_spec}" TARGET ${target})
    add_custom_target(${target}-build-manifest
        COMMAND "${_python}" "${_script}" --spec-pattern "${_dir}/inputs-*.txt"
            --header "${_dir}/manifest.h" --manifest "${_dir}/manifest.json"
        BYPRODUCTS "${_dir}/manifest.h" "${_dir}/manifest.json"
        VERBATIM)
    if(_object_dependencies)
        add_dependencies(${target}-build-manifest ${_object_dependencies})
    endif()
    add_dependencies(${target} ${target}-build-manifest)
    target_compile_definitions(${target} PRIVATE
        "PSX_BUILD_MANIFEST_CONFIG=\"${_dir}/manifest.h\"")
endfunction()

# ROOTS is a list of stable logical-label=absolute-directory pairs. Every file
# must map into one root; unmapped/missing inputs fail instead of disappearing.
# HEADER_DIRECTORIES are explicitly scoped trees, never a repository-wide walk.
# DEPENDENCY_IDENTITIES must name actual immutable pins supplied by the caller;
# merely listing a linked target is not dependency source provenance.
function(psxrecomp_build_manifest target)
    cmake_parse_arguments(B "" "PYTHON" "ROOTS;INPUTS;HEADER_DIRECTORIES;DEPENDENCY_IDENTITIES" ${ARGN})
    if(B_UNPARSED_ARGUMENTS OR NOT TARGET ${target})
        message(FATAL_ERROR "Invalid psxrecomp_build_manifest arguments")
    endif()
    if(NOT B_PYTHON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(B_PYTHON "${Python3_EXECUTABLE}")
    endif()
    get_target_property(_registered ${target} PSX_BUILD_MANIFEST_SCRIPT)
    if(_registered)
        message(FATAL_ERROR "Build manifest already registered for ${target}")
    endif()
    set(_headers "")
    foreach(_dir IN LISTS B_HEADER_DIRECTORIES)
        if(NOT IS_DIRECTORY "${_dir}")
            message(FATAL_ERROR "Build manifest header directory missing: ${_dir}")
        endif()
        file(GLOB_RECURSE _found CONFIGURE_DEPENDS LIST_DIRECTORIES false
            "${_dir}/*.h" "${_dir}/*.hpp" "${_dir}/*.inc" "${_dir}/*.inl")
        list(APPEND _headers ${_found})
    endforeach()
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_PYTHON "${B_PYTHON}")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_SCRIPT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/build_manifest.py")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_ROOTS "${B_ROOTS}")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_HEADERS "${_headers}")
    list(APPEND B_INPUTS "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/build_manifest.py")
    foreach(_language C CXX)
        if(CMAKE_${_language}_COMPILER)
            get_filename_component(_compiler_dir "${CMAKE_${_language}_COMPILER}" DIRECTORY)
            list(APPEND B_ROOTS "compiler-${_language}=${_compiler_dir}")
            list(APPEND B_INPUTS "${CMAKE_${_language}_COMPILER}")
        endif()
    endforeach()
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_ROOTS "${B_ROOTS}")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_INPUTS "${B_INPUTS}")
    set_property(TARGET ${target} PROPERTY PSX_BUILD_MANIFEST_DEPENDENCY_IDENTITIES "${B_DEPENDENCY_IDENTITIES}")
    # Capture target additions and options made by title code after registration.
    cmake_language(EVAL CODE "cmake_language(DEFER CALL _psx_build_manifest_finalize \"${target}\")")
endfunction()
