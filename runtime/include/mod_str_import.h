#pragma once

#include "mod_packages.h"

namespace PSXRecompV4 {

// Mode 2 / 2336, MDEC v2 320x240, stereo 37800 Hz XA, 2x CD.
struct ModStrProfile {
    unsigned fps;
    unsigned max_sectors;
};

bool mod_import_str(const std::filesystem::path& source,
                    const std::filesystem::path& encoder,
                    const std::filesystem::path& cache,
                    ModStrProfile profile, bool cached_only,
                    const ModPrepareProgress& progress,
                    std::filesystem::path& output, std::string& error);
}
