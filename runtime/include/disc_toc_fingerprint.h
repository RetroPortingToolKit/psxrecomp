#pragma once

#include "iso_reader.h"
#include "psx_sha256.h"
#include <cstdio>
#include <sstream>
#include <string>

namespace PSXRecompV4 {

// Versioned geometry identity shared by DiscIdentity/netplay and original-base
// policy. A raw image, its CUE, and lossless CHD use the same mounted track map.
inline std::string disc_toc_fingerprint(PS1::ISOReader& reader) {
    const int count = reader.TrackCount() < 1 ? 1 : reader.TrackCount();
    std::ostringstream toc;
    toc << "psxrecomp-toc-v1\n";
    toc << "tracks=" << count << "\n";
    toc << "leadout=" << reader.GetSectorCount() << "\n";
    for (int t = 1; t <= count; ++t) {
        toc << "t=" << t << ",a=" << (reader.TrackIsAudio(t) ? 1 : 0)
            << ",start=" << reader.TrackStartLBA(t)
            << ",pregap=" << reader.TrackPregapLBA(t) << "\n";
    }
    const std::string canonical=toc.str();
    uint8_t digest[32];
    psx_sha256_compute(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size(), digest);
    char hex[65];
    for (int i=0; i<32; ++i) std::snprintf(hex+i*2,3,"%02x",digest[i]);
    hex[64]='\0';
    return hex;
}

} // namespace PSXRecompV4
