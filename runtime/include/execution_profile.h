#ifndef PSX_EXECUTION_PROFILE_H
#define PSX_EXECUTION_PROFILE_H

#ifdef PSX_EXECUTION_PROFILE_CONFIG
#include PSX_EXECUTION_PROFILE_CONFIG
#else
/* Standalone faithful fixtures and older non-CMake consumers. */
#define PSX_EXECUTION_ENHANCED 0
#define PSX_EXECUTION_NAME "REFERENCE"
#define PSX_EXECUTION_ID ""
#define PSX_EXECUTION_COOKIE 0u
#endif

#endif
