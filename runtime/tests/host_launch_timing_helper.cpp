#include "host_launch_timing.h"

/* Separate translation unit: catches accidentally per-includer ring/parent
 * state, which would hide media-provider work from the runtime TCP reader. */
void host_launch_timing_fixture_nested() {
    PSXRecompV4::HostLaunchTimingScope scope(HOST_LAUNCH_MEDIA_PROVIDER,
                                            "fixture.package", "fixture.feature");
    scope.success();
}
