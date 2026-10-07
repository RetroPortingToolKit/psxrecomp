include_guard(GLOBAL)

# Host utility only: both guest execution profiles retain exactly the same
# file identity. No OpenSSL installation is required for a portable build.
function(psxrecomp_sha256_file target)
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/src/psx_sha256_file.cpp")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/include")
    if(WIN32)
        target_link_libraries(${target} PRIVATE bcrypt)
    else()
        find_package(OpenSSL QUIET COMPONENTS Crypto)
        if(TARGET OpenSSL::Crypto)
            target_compile_definitions(${target} PRIVATE PSX_SHA256_FILE_OPENSSL=1)
            target_link_libraries(${target} PRIVATE OpenSSL::Crypto)
        endif()
    endif()
endfunction()
