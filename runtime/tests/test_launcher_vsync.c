#undef NDEBUG
#include <assert.h>
#include "launcher_vsync.h"
int main(void) {
#ifdef RECOMP_LAUNCHER_HAS_VSYNC_VRR
    assert(psx_vsync_to_launcher(2) == 4);
    assert(psx_vsync_from_launcher(4, 1) == 2);
    assert(psx_vsync_from_launcher(1, 2) == 1);
#endif
    const int modes[] = {1, 0, -1, 2};
    for (int i = 0; i < 4; ++i)
        assert(psx_vsync_from_launcher(psx_vsync_to_launcher(modes[i]), modes[i]) == modes[i]);
    assert(psx_vsync_from_launcher(2, 1) == 0);
    assert(psx_vsync_from_launcher(1, 0) == 1);
    assert(psx_vsync_from_launcher(3, 1) == -1);
    assert(psx_vsync_from_launcher(2, 2) == 0);
    return 0;
}
