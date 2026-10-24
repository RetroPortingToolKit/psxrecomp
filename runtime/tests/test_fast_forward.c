#include "fast_forward.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    for (int tenth = 10; tenth <= 50; ++tenth) {
        const double factor = tenth / 10.0;
        assert(fabs(psx_fast_forward_target(factor, NULL) - factor) < 1e-9);
        const int stride = psx_fast_forward_present_every(factor);
        assert(stride >= 1 && factor / stride <= 1.000001);
    }
    assert(psx_fast_forward_present_every(1.0) == 1);
    assert(psx_fast_forward_target(2.1, "") == 2.1);
    assert(psx_fast_forward_target(4.0, "2.1") == 2.1);
    assert(psx_fast_forward_target(4.0, "16") == 16);
    assert(psx_fast_forward_target(4.0, "max") == -1);
    assert(psx_fast_forward_target(4.0, "0") == -1);
    const char *bad[] = {"1garbage", "nan", "inf", "-2", "17", "0.5"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i)
        assert(psx_fast_forward_target(2.1, bad[i]) == 2.1);
    assert(psx_fast_forward_preference(NAN) == 4);
    assert(psx_fast_forward_preference(0) == 1);
    assert(psx_fast_forward_preference(8) == 5);
    assert(psx_fast_forward_preference(2.14) == 2.1);
    assert(psx_fast_forward_preference(2.16) == 2.2);
    puts("PASS: all 41 fractional speeds, stock 1x presentation and override validation");
    return 0;
}
