#include "host_launch_timing.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

using namespace PSXRecompV4;
void host_launch_timing_fixture_nested();
static int failures;
static void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

int main() {
    static_assert(std::is_trivially_copyable<HostLaunchTimingEvent>::value,
                  "C-facing snapshot must remain POD");
    HostLaunchTimingEvent events[HOST_LAUNCH_TIMING_CAPACITY]{};
    uint64_t total = 0, overwritten = 0;
    {
        HostLaunchTimingScope parent(HOST_LAUNCH_COMMIT);
        host_launch_timing_fixture_nested();
        parent.success();
    }
    auto n = host_launch_timing_ring().snapshot(events, 256, &total, &overwritten);
    check(n == 2 && total == 2 && overwritten == 0, "shared ring across translation units");
    check(events[0].parent_id == events[1].scope_id && events[1].parent_id == 0,
          "shared nested parent across translation units");
    check(events[0].scope_id > events[1].scope_id && events[0].seq < events[1].seq,
          "completion order differs from start order");
    check(events[0].start_us >= events[1].start_us &&
          events[0].start_us + events[0].duration_us <=
              events[1].start_us + events[1].duration_us, "child time inside parent");
    check(events[0].ok && events[1].ok &&
          std::string(events[0].package_id) == "fixture.package", "success and metadata retained");

    HostLaunchTimingRing ring;
    try {
        HostLaunchTimingScope failed(HOST_LAUNCH_PREPARE_RESOURCES, nullptr, nullptr, ring);
        HostLaunchTimingScope child(HOST_LAUNCH_MEDIA_PROVIDER, nullptr, nullptr, ring);
        throw std::runtime_error("fixture failure");
    } catch (const std::runtime_error&) { }
    {
        const std::string long_label(100, 'x');
        HostLaunchTimingScope next(HOST_LAUNCH_RESOLVE, long_label.c_str(), "short", ring);
        next.success();
    }
    n = ring.snapshot(events, 256, &total, &overwritten);
    check(n == 3 && !events[0].ok && !events[1].ok && events[2].ok,
          "exception exits remain failures and next call succeeds");
    check(events[0].parent_id == events[1].scope_id && events[2].parent_id == 0,
          "exception restores enclosing context");
    check(events[2].labels_truncated && std::strlen(events[2].package_id) == 63 &&
          std::string(events[2].feature_id) == "short", "bounded labels report truncation");

    for (unsigned i = 0; i < HOST_LAUNCH_TIMING_CAPACITY; ++i) {
        HostLaunchTimingScope scope(HOST_LAUNCH_DISC_HASH, nullptr, nullptr, ring);
        scope.success();
    }
    n = ring.snapshot(events, 256, &total, &overwritten);
    check(n == 256 && total == 259 && overwritten == 3, "wrap retains fixed capacity");
    for (uint32_t i = 1; i < n; ++i)
        check(events[i].seq == events[i - 1].seq + 1, "wrapped snapshot oldest first");
    n = ring.snapshot(events, 2, &total, &overwritten);
    check(n == 2 && events[0].seq == 258 && events[1].seq == 259,
          "small reader receives newest entries without draining");
    check(ring.snapshot(nullptr, 256, &total, &overwritten) == 0 && total == 259,
          "metadata query does not consume entries");

    HostLaunchTimingRing concurrent;
    auto writer = [&]() {
        for (unsigned i = 0; i < 40; ++i) {
            HostLaunchTimingScope parent(HOST_LAUNCH_COMMIT, nullptr, nullptr, concurrent);
            HostLaunchTimingScope child(HOST_LAUNCH_RESOLVE, nullptr, nullptr, concurrent);
            child.success(); parent.success();
        }
    };
    std::thread a(writer), b(writer);
    a.join(); b.join();
    n = concurrent.snapshot(events, 256, &total, &overwritten);
    check(n == 160 && total == 160 && overwritten == 0, "concurrent completion count preserved");
    for (uint32_t i = 0; i < n; ++i) {
        check(events[i].seq == i + 1 && events[i].ok, "concurrent sequence publication");
        if (events[i].stage == HOST_LAUNCH_COMMIT)
            check(events[i].parent_id == 0, "thread-local roots do not inherit other writer");
        else {
            bool found = false;
            for (uint32_t j = i + 1; j < n; ++j)
                if (events[j].scope_id == events[i].parent_id &&
                    events[j].stage == HOST_LAUNCH_COMMIT) found = true;
            check(found, "concurrent child belongs to a completed parent");
        }
    }
    return failures ? 1 : 0;
}
