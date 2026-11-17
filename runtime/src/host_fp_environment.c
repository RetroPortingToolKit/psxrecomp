#include "host_fp_environment.h"
#include <fenv.h>
#if defined(__x86_64__) || defined(_M_X64)
/* SSE2 is part of the x64 execution ABI, not an optional instruction here.
 * Other targets retain the standard C path without x86 imports. */
#include <xmmintrin.h>
#define PSX_FP_X64 1
#else
#define PSX_FP_X64 0
#endif

int psx_host_fp_environment_is_default(void) {
    if (fegetround()!=FE_TONEAREST) return 0;
#if PSX_FP_X64
    // Ignore accrued status flags. Mask exceptions, round to nearest-even,
    // disable FTZ/DAZ; never enable a possibly unsupported DAZ bit.
    if ((_mm_getcsr()&0xFFC0u)!=0x1F80u) return 0;
#endif
    return 1;
}
int psx_host_fp_environment_set_default(void) {
    if (fesetenv(FE_DFL_ENV)) return 0;
#if PSX_FP_X64
    _mm_setcsr((_mm_getcsr()&~0xFFC0u)|0x1F80u);
#endif
    return psx_host_fp_environment_is_default();
}
int psx_host_fp_environment_contract(void) {
    if (!psx_host_fp_environment_is_default()) return 0;
    return PSX_FP_X64?2:1;
}
