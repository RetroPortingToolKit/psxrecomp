#include <cstdint>

// These isolated instruction/session fixtures have no allocated mod memory.
// The packet-shadow fixture supplies its own bounded, allocated aperture.
extern "C" int psx_mod_gpu_dma_memory_contains(uint32_t, uint32_t) { return 0; }
