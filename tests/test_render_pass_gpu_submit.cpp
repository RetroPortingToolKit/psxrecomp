#ifdef NDEBUG
#undef NDEBUG
#endif
#include "render_pass_gpu_submit.hpp"
#include "dma_gpu_ll.h"
#include "packet_address_index.h"
#include <cassert>
#include <cstdio>
#include <tuple>
#include <vector>

using Event=std::tuple<char,uint32_t,uint32_t>;
static uint32_t ram[256]{};
static std::vector<Event> trace;
extern "C" {
int g_psx_render_pass_active=1;
uint32_t g_psx_ram_size=sizeof ram,g_psx_ram_mask=sizeof ram-1;
uint32_t psx_mod_gpu_dma_resolve_address(uint32_t a){return a&g_psx_ram_mask&~3u;}
uint32_t psx_mod_read_word(uint32_t a){return ram[(a&g_psx_ram_mask)/4];}
void gpu_ws_begin_linked_list(){trace.emplace_back('B',0,0);}
void gpu_ws_end_linked_list(){trace.emplace_back('F',0,0);}
void gpu_ws_prepass_linked_list(uint32_t a){trace.emplace_back('P',a,0);}
void gpu_ws_validate_linked_list_header(uint32_t a,uint32_t h){trace.emplace_back('H',a,h);}
void gpu_ws_validate_linked_list_node(uint32_t a,uint32_t n){trace.emplace_back('V',a,n);}
void gpu_set_gp0_linked_list_node(uint32_t a,uint32_t n){trace.emplace_back('N',a,n);}
void gpu_set_gp0_source(uint32_t a){trace.emplace_back('S',a,0);}
void gpu_write_gp0(uint32_t w){trace.emplace_back('W',w,0);}
}
static uint32_t resolve(void*,uint32_t a){return psx_mod_gpu_dma_resolve_address(a);}
static uint32_t read(void*,uint32_t a){return psx_mod_read_word(a);}
static void header(void*,uint32_t a,uint32_t h){gpu_ws_validate_linked_list_header(a,h);}
static int node(void*,uint32_t a,uint32_t n){gpu_ws_validate_linked_list_node(a,n);gpu_set_gp0_linked_list_node(a,n);return 1;}
static void emit(void*,uint32_t a,uint32_t w){gpu_set_gp0_source(a);gpu_write_gp0(w);}
static void complete(void*,int limited){assert(!limited);gpu_ws_end_linked_list();}
int main(){
    if constexpr(!PSXRenderSubmit::enhanced) {
        assert(!PSXRenderSubmit::ot(0x40));assert(trace.empty());
        puts("PASS: REFERENCE leaves GPU submission to the original SDK");return 0;
    }
    // Empty OT links, a multi-command environment, a primitive, and a wrapped
    // RAM payload: compare every command, source address and guard event with
    // the real LLE DMA walker (only scheduling is deliberately different).
    ram[0x40/4]=0x00000080;ram[0x80/4]=0x030003FC;
    ram[0x84/4]=0xE1000020;ram[0x88/4]=0xE3000000;ram[0x8C/4]=0xE40003FF;
    ram[0x3FC/4]=0x02FFFFFF;ram[0]=0x6000FF00;ram[1]=0x00100010;
    const DMAGPULinkedListOps ops={resolve,read,header,node,emit,complete};
    DMAGPULinkedList ll{};gpu_ws_begin_linked_list();gpu_ws_prepass_linked_list(0x40);
    dma_gpu_ll_start(&ll,0x40,32768);dma_gpu_ll_advance(&ll,UINT32_MAX,&ops,nullptr);
    assert(!ll.active);const auto expected=trace;trace.clear();
    assert(PSXRenderSubmit::ot(0x80000040));assert(trace==expected);
    trace.clear();ram[0x80/4]=0x03000040;
    assert(!PSXRenderSubmit::ot(0x40));assert(trace.empty());
    g_psx_render_pass_active=0;assert(!PSXRenderSubmit::ot(0x40));assert(trace.empty());

    // Collision and duplicate lookup must preserve the old first-match scan,
    // including misses and clearing between generations.
    PSXPacketAddressIndex slots[256]{};uint32_t addresses[120];
    for(unsigned i=0;i<120;++i){addresses[i]=((i*17)%97)*1024;psx_packet_address_index_add(slots,256,addresses[i],i);}
    for(unsigned i=0;i<120;++i){unsigned first=0;while(addresses[first]!=addresses[i])++first;
        assert(psx_packet_address_index_find(slots,256,addresses[i])==first);}
    assert(psx_packet_address_index_find(slots,256,1)==UINT32_MAX);
    psx_packet_address_index_clear(slots,256);
    assert(psx_packet_address_index_find(slots,256,addresses[0])==UINT32_MAX);
    puts("PASS: HLE command/provenance trace matches LLE DMA; cycle/outside-pass refusal; indexed guards match first scan");
}
