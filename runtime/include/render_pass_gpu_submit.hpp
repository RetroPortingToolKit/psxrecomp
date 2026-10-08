#pragma once
#include "execution_profile.h"
#include "gpu.h"
#include "mod_memory.h"
#include "mod_plugins.h"
#include "psx_cycle_freeze.h"
#include <unordered_set>
#include <vector>

/* ENHANCED submission for a frozen render sandbox. The scene owns only the
 * resulting picture: PsyQ queue bookkeeping/IRQ polling is restored at exit.
 * REFERENCE continues to call the original SDK. Use the exact DMA provenance
 * and widescreen prepass services; feeding bare GP0 words loses PGXP/UI data.
 * Main RAM and explicitly allocated primitive arenas. Refuse an unsupported
 * graph or an arena payload outside its allocation before GPU output.
 */
class PSXRenderSubmit {
    struct Node { uint32_t address,header; };
public:
    static constexpr bool enhanced=PSX_EXECUTION_ENHANCED != 0;
    static bool ot(uint32_t root) {
#if PSX_EXECUTION_ENHANCED
        if(!g_psx_render_pass_active)return false;
        // Persistent scratch tolerates a render-pass watchdog longjmp.
        static std::vector<Node> nodes;
        static std::unordered_set<uint32_t> seen;
        nodes.clear();seen.clear();
        uint32_t address=psx_mod_gpu_dma_resolve_address(root);
        for(unsigned count=0;count<32768;++count) {
            if((address>=psx_ram_live_bytes() && !psx_mod_gpu_dma_memory_contains(address,4)) ||
               (address&3) || !seen.insert(address).second)return false;
            const uint32_t header=psx_mod_read_word(address),words=header>>24;
            // The aperture must not fold an overrun into retail RAM. Retail
            // payload words still wrap through the DMAC's live RAM geometry.
            if(address>=psx_ram_live_bytes() &&
               !psx_mod_gpu_dma_memory_contains(address,(words+1)*4))return false;
            for(uint32_t i=1;i<=words;++i)
                if(psx_mod_gpu_dma_resolve_address(address+4*i)>=psx_ram_live_bytes() &&
                   !psx_mod_gpu_dma_memory_contains(address+4*i,4))return false;
            nodes.push_back({address,header});
            if((header&0xFFFFFF)==0xFFFFFF)break;
            address=psx_mod_gpu_dma_resolve_address(header&0xFFFFFF);
        }
        if(nodes.empty() || (nodes.back().header&0xFFFFFF)!=0xFFFFFF)return false;
        gpu_ws_begin_linked_list();
        gpu_ws_prepass_linked_list(psx_mod_gpu_dma_resolve_address(root));
        for(const Node& node:nodes) {
            gpu_ws_validate_linked_list_header(node.address,node.header);
            const uint32_t words=node.header>>24;
            gpu_ws_validate_linked_list_node(node.address,words);
            gpu_set_gp0_linked_list_node(node.address,words);
            for(uint32_t i=1;i<=words;++i) {
                address=psx_mod_gpu_dma_resolve_address(node.address+4*i);
                gpu_set_gp0_source(address);
                gpu_write_gp0(psx_mod_read_word(address));
            }
        }
        gpu_ws_end_linked_list();
        return true;
#else
        (void)root;return false;
#endif
    }
};
