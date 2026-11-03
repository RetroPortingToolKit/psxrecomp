include_guard(GLOBAL)

# Release channel: the one input that says what a release-package build is.
#
#   PSX_RELEASE_CHANNEL=public    public production release
#   PSX_RELEASE_CHANNEL=playtest  non-public playtest / pre-release package
#   unset (empty)                 a dev or test build; never packaged
#
# tools/package_title_release.py is the only supported way to cut a release.
# It requires --channel public|playtest (no default), configures with
# -DPSX_RELEASE_CHANNEL=<channel>, and refuses to stage a build tree whose
# channel is empty or different. PSX_PUBLIC_RELEASE is DERIVED from the
# channel here (public => ON); it is no longer an input. Before this module,
# a packaging configure that forgot -DPSX_PUBLIC_RELEASE=ON silently produced
# a playtest build (diagnostics on) under a public name.
#
# Backstop rules (FATAL_ERROR):
#   * a channel other than public/playtest;
#   * -DPSX_PUBLIC_RELEASE=ON without a channel -- the pre-channel way to ask
#     for a public package; name the channel instead;
#   * a PSX_PUBLIC_RELEASE value that contradicts the channel.
# An explicit or cached PSX_PUBLIC_RELEASE=OFF without a channel is the old
# option's default (every dev tree configured before this module has it) and
# stays a dev build.
set(PSX_RELEASE_CHANNEL "" CACHE STRING
    "Release package channel: public or playtest (empty = dev build, not packageable)")
set_property(CACHE PSX_RELEASE_CHANNEL PROPERTY STRINGS ";public;playtest")
if(NOT PSX_RELEASE_CHANNEL STREQUAL "" AND
   NOT PSX_RELEASE_CHANNEL MATCHES "^(public|playtest)$")
    message(FATAL_ERROR
        "PSX_RELEASE_CHANNEL must be 'public' or 'playtest' (got '${PSX_RELEASE_CHANNEL}'). "
        "Cut releases with psxrecomp/tools/package_title_release.py --channel public|playtest.")
endif()

if(DEFINED CACHE{PSX_PUBLIC_RELEASE})
    get_property(_psx_public_type CACHE PSX_PUBLIC_RELEASE PROPERTY TYPE)
    if(NOT _psx_public_type STREQUAL "INTERNAL")
        # Set by the caller (-D) or left by a configure that predates channels.
        if(PSX_PUBLIC_RELEASE AND PSX_RELEASE_CHANNEL STREQUAL "")
            message(FATAL_ERROR
                "PSX_PUBLIC_RELEASE=ON without a release channel. A packaging build must "
                "name its channel: -DPSX_RELEASE_CHANNEL=public|playtest (PSX_PUBLIC_RELEASE "
                "is derived from it). Releases are cut with "
                "psxrecomp/tools/package_title_release.py --channel public|playtest. "
                "To reuse this tree as a dev build, reconfigure with -UPSX_PUBLIC_RELEASE.")
        endif()
        if((PSX_PUBLIC_RELEASE AND PSX_RELEASE_CHANNEL STREQUAL "playtest") OR
           (NOT PSX_PUBLIC_RELEASE AND PSX_RELEASE_CHANNEL STREQUAL "public"))
            message(FATAL_ERROR
                "PSX_PUBLIC_RELEASE=${PSX_PUBLIC_RELEASE} contradicts "
                "PSX_RELEASE_CHANNEL=${PSX_RELEASE_CHANNEL}. Pass only the channel "
                "(reconfigure with -UPSX_PUBLIC_RELEASE).")
        endif()
    endif()
    unset(_psx_public_type)
endif()

if(PSX_RELEASE_CHANNEL STREQUAL "public")
    set(_psx_public_release ON)
else()
    set(_psx_public_release OFF)
endif()
set(PSX_PUBLIC_RELEASE ${_psx_public_release} CACHE INTERNAL
    "Derived from PSX_RELEASE_CHANNEL (public => ON); not an input")
unset(_psx_public_release)
if(PSX_RELEASE_CHANNEL STREQUAL "")
    message(STATUS "psxrecomp: release channel: none (dev build; not packageable)")
else()
    message(STATUS "psxrecomp: release channel: ${PSX_RELEASE_CHANNEL} "
                   "(PSX_PUBLIC_RELEASE=${PSX_PUBLIC_RELEASE})")
endif()

# Optional exact source commit for build identities. The packager passes the
# title's HEAD; a tree configured from WSL cannot always ask git itself (a
# Windows worktree's .git names an F:/ path). Empty = the title decides.
set(PSX_SOURCE_COMMIT "" CACHE STRING
    "Exact title source commit for build identities (set by the release packager)")
string(LENGTH "${PSX_SOURCE_COMMIT}" _psx_source_commit_length)
if(NOT PSX_SOURCE_COMMIT STREQUAL "" AND
   (NOT PSX_SOURCE_COMMIT MATCHES "^[0-9a-f]+$" OR NOT _psx_source_commit_length EQUAL 40))
    message(FATAL_ERROR "PSX_SOURCE_COMMIT must be a full 40-hex commit (got '${PSX_SOURCE_COMMIT}')")
endif()
unset(_psx_source_commit_length)
