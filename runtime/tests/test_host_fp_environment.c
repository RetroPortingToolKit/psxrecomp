#include "host_fp_environment.h"
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#define TEST_X64 1
#else
#define TEST_X64 0
#endif
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
int main(void) {
    fenv_t original;
    CHECK(!fegetenv(&original));
    CHECK(!fesetround(FE_DOWNWARD));
    CHECK(!psx_host_fp_environment_is_default());
    volatile float half=1.5f;
    CHECK(nearbyintf(half)==1.0f);
#if TEST_X64
    // FTZ is architected SSE; leave DAZ disabled, including on old CPUs.
    _mm_setcsr(_mm_getcsr()|0x8000u);
#endif
    CHECK(psx_host_fp_environment_set_default());
    CHECK(psx_host_fp_environment_is_default());
    CHECK(psx_host_fp_environment_contract()==(TEST_X64?2:1));
    CHECK(nearbyintf(half)==2.0f);
#if TEST_X64
    volatile float normal=FLT_MIN, quarter=0.25f;
    volatile float subnormal=normal*quarter;
    CHECK(subnormal==FLT_MIN*0.25f); // Actual gradual-underflow arithmetic.
    _mm_setcsr(_mm_getcsr()|0x8000u);
    CHECK(!psx_host_fp_environment_is_default());
    CHECK(psx_host_fp_environment_set_default());
#endif
    CHECK(psx_host_fp_environment_set_default()); // Idempotent.
    CHECK(!fesetenv(&original));
    puts("host FP environment: rounding, x64 controls and gradual underflow PASS");
    return 0;
}
