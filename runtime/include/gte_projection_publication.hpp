#pragma once
#include "gte_postfx_projection.h"
#include <atomic>
/* CPU owns cached fields; renderer owns enable(). Atomic payloads plus an SC
 * version guard make concurrent reads defined and reject mixed tuples. */
class GteProjectionPublication {
    std::atomic<uint32_t> control{0}, version{0}, published{0};
    std::atomic<int32_t> h{0}, x{0}, y{0};
    uint32_t seen=0; GtePostfxProjection cached{};
public:
    void enable(bool on) {
        const auto c=control.load();
        if(bool(c&1)!=on) control.store(((c+2)&~1u)|uint32_t(on));
    }
    void publish(int32_t ph,int32_t px,int32_t py) {
        const auto c=control.load(std::memory_order_relaxed);
        if(!(c&1))return;
        if(c==seen && ph==cached.h && px==cached.ofx && py==cached.ofy)return;
        version.fetch_add(1); h.store(ph); x.store(px); y.store(py); published.store(c);
        version.fetch_add(1); seen=c; cached={ph,px,py};
    }
    bool read(GtePostfxProjection& out) const {
        const auto c=control.load(); if(!(c&1))return false;
        for(unsigned i=0;i<3;++i) {
            const auto v=version.load(); if(v&1)continue;
            const auto p=published.load(); GtePostfxProjection value{h.load(),x.load(),y.load()};
            if(v==version.load() && p==c && c==control.load()) {out=value;return true;}
        }
        return false;
    }
};
