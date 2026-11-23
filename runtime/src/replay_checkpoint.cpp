#include "replay_file.h"
#include "host_sha256.h"
#include "psx_sha256.h"

void replay_file_checkpoint(uint32_t frame, const uint8_t *ram, uint64_t cycle,
                            ReplayCheckpoint *out)
{
    constexpr uint32_t page_bytes = 4096;
    constexpr uint32_t ram_bytes = page_bytes * REPLAY_FILE_RAM_PAGES;

    out->frame = frame;
    out->cycle = cycle;
    if (!PSXRecompV4::host_sha256_bytes(ram, ram_bytes, out->ram_sha256))
        psx_sha256_compute(ram, ram_bytes, out->ram_sha256);

    for (uint32_t p = 0; p < REPLAY_FILE_RAM_PAGES; ++p) {
        uint64_t hash = UINT64_C(14695981039346656037);
        const uint8_t *bytes = ram + p * page_bytes;
        for (uint32_t i = 0; i < page_bytes; ++i)
            hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
        out->pages[p] = hash;
    }
}
