# Optional host headset backend. Faithful builds have no SDK dependency.
option(PSX_OPENXR "Build experimental Win32/OpenGL OpenXR backend" OFF)
if(PSX_OPENXR)
    if(NOT WIN32)
        message(FATAL_ERROR "PSX_OPENXR currently supports Win32/OpenGL")
    endif()
    include(FetchContent)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_LOADER ON CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(psx_openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG b76b80adaf65ac3ad6cc1ce61974fb29a5d02352)
    FetchContent_MakeAvailable(psx_openxr_sdk)
    if(MINGW)
        # Older MinGW desktop headers omit WINAPI_PARTITION_SYSTEM. The SDK
        # correctly treats it as zero; leave this vendor warning nonfatal.
        target_compile_options(openxr_loader PRIVATE -Wno-error=undef)
    endif()
endif()
