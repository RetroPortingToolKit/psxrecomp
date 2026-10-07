#pragma once
#include "render_pass_replay.hpp"
#include "gpu.h"
#include <utility>

/* Preserve packets prepended after a title's world draw, and retarget the
 * completed OT's screen environment to the impending display bank. Operates
 * only on bounded, RAM-backed lists; command payloads are decoded, never
 * searched for byte patterns which could also be colors or texture data. */
class PSXOTReplay {
    struct Packet { uint32_t address; std::vector<uint32_t> words; };
    struct Bucket { uint32_t address, head; std::vector<Packet> late; };
    std::vector<Bucket> buckets;
    static uint32_t rd(uint32_t p) { return psx_mod_read_word(p); }
    static void wr(uint32_t p,uint32_t v) { psx_mod_write_word(p,v); }
    static bool valid(uint32_t p,uint32_t bytes) {
        return !(p&3) && p>=0x10000 && p<=memory_get_ram_bytes() &&
            bytes<=memory_get_ram_bytes()-p;
    }
public:
    void reset() { buckets.clear(); }
    bool capture(uint32_t first,uint32_t count) {
        reset();first&=0x1FFFFFFF;
        if(count>8192 || !valid(first,count*4))return false;
        buckets.reserve(count);
        for(uint32_t i=0;i<count;++i)buckets.push_back({first+4*i,rd(first+4*i)&0xFFFFFF,{}});
        return true;
    }
    // A title may build a separate single-head HUD list after world capture.
    // Its authored initial value is the terminator, independent of the old
    // frame's contents currently occupying that stack or arena slot.
    bool capture_new_list(uint32_t first) {
        reset();first&=0x1FFFFFFF;
        if(!valid(first,4))return false;
        buckets.push_back({first,0xFFFFFF,{}});return true;
    }
    bool preserve_late() {
        uint32_t nodes=0,words=0;
        for(auto& b:buckets) {
            b.late.clear();uint32_t node=rd(b.address)&0xFFFFFF;
            while(node!=b.head) {
                if(!valid(node,4) || ++nodes>8192)return false;
                const uint32_t tag=rd(node),n=1+(tag>>24);
                words+=n;
                if(words>262144 || !valid(node,n*4))return false;
                Packet p{node,{}};p.words.reserve(n);
                for(uint32_t j=0;j<n;++j)p.words.push_back(rd(node+4*j));
                b.late.push_back(std::move(p));node=tag&0xFFFFFF;
            }
        }
        return !buckets.empty();
    }
    void apply_late() const {
        for(const auto& b:buckets) {
            if(b.late.empty())continue;
            const uint32_t head=rd(b.address);
            for(const auto& p:b.late)for(uint32_t j=0;j<p.words.size();++j)wr(p.address+4*j,p.words[j]);
            const auto& tail=b.late.back();
            wr(tail.address,(tail.words[0]&0xFF000000)|(head&0xFFFFFF));
            wr(b.address,(head&0xFF000000)|b.late.front().address);
        }
    }
    static bool retarget_y(uint32_t tail,int from,int to,int height) {
        if(!g_psx_render_pass_active || height<=0 || from<0 || to<0 ||
           from+height>512 || to+height>512)return false;
        uint32_t node=tail&0x1FFFFFFF,nodes=0,total=0;
        std::vector<std::pair<uint32_t,uint32_t>> edits;
        for(;;) {
            if(!valid(node,4) || ++nodes>65536)return false;
            const uint32_t tag=rd(node),n=tag>>24;
            total+=n;
            if(total>1048576 || !valid(node,4+4*n))return false;
            for(uint32_t i=0;i<n;) {
                const uint32_t p=node+4+4*i,w=rd(p),op=w>>24;
                int count=gpu_gp0_command_word_count(uint8_t(op));
                if(count<0) {
                    // Both mono and shaded polylines have at least two
                    // vertices; shaded terminators occupy the next color.
                    const uint32_t stride=(op&0x10)?2:1;
                    uint32_t end=i+((op&0x10)?4:3);
                    for(;end<n;end+=stride)if((rd(node+4+4*end)&0xF000F000)==0x50005000)break;
                    if(end>=n)return false;
                    count=int(end-i+1);
                }
                if(count<=0 || i+uint32_t(count)>n)return false;
                if(op>=0xA0 && op<=0xBF) {
                    const uint32_t wh=rd(p+8);
                    const uint32_t width=((wh-1)&1023)+1, rows=(((wh>>16)-1)&511)+1;
                    count+=int((width*rows+1)/2);
                    if(i+uint32_t(count)>n)return false;
                }
                if(op==0xE3 || op==0xE4) {
                    const int y=int((w>>10)&511);
                    if(y>=from && y<from+height)
                        edits.emplace_back(p,(w&~(511u<<10))|(uint32_t(y+to-from)<<10));
                } else if(op==0xE5) {
                    int y=int((w>>11)&2047);if(y&1024)y-=2048;
                    // Draw offsets may include authored camera shake.
                    if(y>=from-128 && y<from+height)
                        edits.emplace_back(p,(w&~(2047u<<11))|((uint32_t(y+to-from)&2047)<<11));
                }
                i+=uint32_t(count);
            }
            node=tag&0xFFFFFF;
            if(node==0xFFFFFF)break;
        }
        for(const auto& e:edits)wr(e.first,e.second);
        return true;
    }
};
