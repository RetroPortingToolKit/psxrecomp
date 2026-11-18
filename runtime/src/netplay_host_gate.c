#include "netplay_host_gate.h"
#include "psx_sha256.h"
#include <string.h>
static uint32_t word(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static PSXHostProfileCatalog catalog(NpHostGate *g){PSXHostProfileCatalog c={g->rules,0};while(c.count<8&&g->rules[c.count].profile[0])++c.count;return c;}
static unsigned dense(const NpHostGate *g,unsigned slot){unsigned n=0;for(unsigned i=0;i<slot;++i)if(g->occupied&(1u<<i))++n;return n;}
static void update(NpHostGate *g){
    if(g->error)return;
    if(g->known==g->occupied && !g->match.sealed){
        PSXHostCapabilities peers[9];unsigned count=0;
        for(unsigned i=0;i<9;++i)if(g->occupied&(1u<<i))peers[count++]=g->capabilities[i];
        PSXHostProfileCatalog cat=catalog(g);
        uint8_t bound[32],mask[4];put(mask,g->occupied);
        psx_sha256_ctx sha;psx_sha256_init(&sha);const char salt[]="psx-host-match-nonces-v1";
        psx_sha256_update(&sha,(const uint8_t *)salt,sizeof salt-1);psx_sha256_update(&sha,g->session,32);
        psx_sha256_update(&sha,mask,4);
        for(unsigned i=0;i<9;++i)if(g->occupied&(1u<<i)){uint8_t slot=(uint8_t)i;
            psx_sha256_update(&sha,&slot,1);psx_sha256_update(&sha,g->peers[i].wire+4,16);}
        psx_sha256_final(&sha,bound);
        int result=psx_host_match_select(&g->match,peers,count,&cat,bound);
        if(result!=PSX_HOST_OK){g->error=result==PSX_HOST_NO_COMMON?NP_HOST_NO_COMMON:result==PSX_HOST_BUILD_MISMATCH?NP_HOST_ARTIFACT:NP_HOST_BAD_DATA;return;}
        if(g->observer){
            int allowed=0;
            for(unsigned i=0;i<g->observer_capabilities.offer_count;++i){
                const PSXHostOffer *o=&g->observer_capabilities.offers[i];
                if(!strcmp(o->profile,g->match.selected.profile)&&o->revision==g->match.selected.revision&&
                   !memcmp(o->qualification,g->match.selected.qualification,32)&&
                   (g->match.selected.flags&PSX_HOST_PROFILE_OBSERVER))allowed=1;
            }
            if(!allowed){g->error=NP_HOST_NO_COMMON;return;}
        }else{
            psx_host_match_confirm(&g->match,dense(g,g->local),g->match.selected.identity);
            g->confirmed|=1u<<g->local;
        }
    }
    if(!g->match.sealed)return;
    for(unsigned i=0;i<9;++i)if((g->occupied&(1u<<i))&&i!=g->local&&g->peers[i].selection_chunks==15){
        if(!psx_host_match_confirm(&g->match,dense(g,i),g->peers[i].selection)){g->error=NP_HOST_CONFIRM;return;}
        g->confirmed|=1u<<i;
    }
}
int np_host_gate_init(NpHostGate *g,const PSXHostCapabilities *caps,const PSXHostProfileCatalog *cat,uint32_t occupied,uint32_t local,const uint8_t session[32],const uint8_t nonce[16]){
    if(!g)return 0;
    memset(g,0,sizeof *g);
    unsigned identity=0,fresh=0;if(session)for(unsigned i=0;i<32;++i)identity|=session[i];
    if(nonce)for(unsigned i=0;i<16;++i)fresh|=nonce[i];
    if(local>=9 || (occupied&~0x1FFu)||!(occupied&(1u<<local))||!session||!caps||!cat||
       !identity||!fresh||!psx_host_caps_validate(caps,cat)||!caps->offer_count){g->error=NP_HOST_BAD_DATA;return 0;}
    unsigned count=0;for(unsigned i=0;i<9;++i)if(occupied&(1u<<i))++count;
    if(count<2){g->error=NP_HOST_BAD_DATA;return 0;}
    g->enabled=1;g->local=local;g->occupied=occupied;g->known=1u<<local;memcpy(g->session,session,32);
    memcpy(g->rules,cat->rules,cat->count*sizeof *cat->rules);g->capabilities[local]=*caps;
    /* Canonical offer order, independent of registration/catalog insertion. */
    PSXHostCapabilities *c=g->capabilities+local;
    for(unsigned i=0;i<c->offer_count;++i)for(unsigned j=i+1;j<c->offer_count;++j)
        if(strcmp(c->offers[i].profile,c->offers[j].profile)>0){PSXHostOffer tmp=c->offers[i];c->offers[i]=c->offers[j];c->offers[j]=tmp;}
    NpHostPeer *p=g->peers+local;memcpy(p->wire,"PHC2",4);memcpy(p->wire+4,nonce,16);
    p->bytes=20+(uint32_t)psx_host_caps_encode(c,p->wire+20,sizeof p->wire-20);
    p->chunks=(p->bytes+11)/12;
    return p->bytes!=0;
}
int np_host_gate_observer_init(NpHostGate *g,const PSXHostCapabilities *caps,const PSXHostProfileCatalog *cat,uint32_t occupied,const uint8_t session[32]){
    if(!g)return 0;memset(g,0,sizeof *g);
    unsigned identity=0,count=0;if(session)for(unsigned i=0;i<32;++i)identity|=session[i];
    for(unsigned i=0;i<9;++i)if(occupied&(1u<<i))++count;
    if(!session||!identity||!caps||!cat||count<2||(occupied&~0x1FFu)||
       !caps->offer_count||!psx_host_caps_validate(caps,cat)){g->error=NP_HOST_BAD_DATA;return 0;}
    g->enabled=1;g->observer=1;g->local=9;g->occupied=occupied;
    g->observer_capabilities=*caps;memcpy(g->session,session,32);
    memcpy(g->rules,cat->rules,cat->count*sizeof *cat->rules);return 1;
}
int np_host_gate_note(NpHostGate *g,const NpHostPacket *packet,int authoritative_from){
    if(!g||!packet||!g->enabled || (packet->epoch!=NP_HOST_CAPS_MAGIC&&packet->epoch!=NP_HOST_SELECT_MAGIC&&packet->epoch!=NP_HOST_ERROR_MAGIC))return 0;
    if(g->error)return 1;
    unsigned slot=packet->slot;
    if(authoritative_from<0||authoritative_from>=9||(unsigned)authoritative_from!=slot){g->error=NP_HOST_BAD_DATA;return 1;}
    if(slot>=9||slot==g->local||!(g->occupied&(1u<<slot)))return 1;
    if(packet->epoch==NP_HOST_ERROR_MAGIC){g->error=NP_HOST_REMOTE;return 1;}
    NpHostPeer *p=g->peers+slot;
    if(packet->epoch==NP_HOST_CAPS_MAGIC){
        unsigned index=packet->flags;
        if(index>=NP_HOST_FRAGMENTS){g->error=NP_HOST_BAD_DATA;return 1;}
        uint8_t bytes[12];put(bytes,packet->a);put(bytes+4,packet->b);put(bytes+8,packet->seen);
        uint64_t bit=(uint64_t)1<<index;
        if(p->fragments&bit){if(memcmp(p->wire+index*12,bytes,12))g->error=NP_HOST_CONFLICT;return 1;}
        memcpy(p->wire+index*12,bytes,12);p->fragments|=bit;
        if((p->fragments&7)==7){
            unsigned fresh=0;for(unsigned i=4;i<20;++i)fresh|=p->wire[i];
            if(!fresh||memcmp(p->wire,"PHC2",4)||memcmp(p->wire+20,"PCAP",4)||p->wire[24]!=1||p->wire[27]>8){g->error=NP_HOST_BAD_DATA;return 1;}
            p->bytes=20+56+72u*p->wire[27];p->chunks=(p->bytes+11)/12;
            uint64_t all=((uint64_t)1<<p->chunks)-1;
            if(p->fragments&~all){g->error=NP_HOST_BAD_DATA;return 1;}
            if(p->fragments==all){
                for(unsigned i=p->bytes;i<p->chunks*12;++i)if(p->wire[i]){g->error=NP_HOST_BAD_DATA;return 1;}
                PSXHostCapabilities *c=g->capabilities+slot;
                if(!psx_host_caps_decode(c,p->wire+20,p->bytes-20)){g->error=NP_HOST_BAD_DATA;return 1;}
                for(unsigned i=1;i<c->offer_count;++i)if(strcmp(c->offers[i-1].profile,c->offers[i].profile)>=0){g->error=NP_HOST_BAD_DATA;return 1;}
                if(memcmp(c->build,g->observer?g->observer_capabilities.build:g->capabilities[g->local].build,32)){g->error=NP_HOST_ARTIFACT;return 1;}
                g->known|=1u<<slot;
            }
        }
    }else{
        unsigned index=packet->flags;
        if(index>=4||(packet->seen&~g->occupied)){g->error=NP_HOST_BAD_DATA;return 1;}
        uint8_t bytes[8];put(bytes,packet->a);put(bytes+4,packet->b);
        if(p->selection_chunks&(1u<<index)){
            if(memcmp(p->selection+index*8,bytes,8)){g->error=NP_HOST_CONFLICT;return 1;}
        }else{memcpy(p->selection+index*8,bytes,8);p->selection_chunks|=1u<<index;}
        if((packet->seen&g->occupied)==g->occupied)g->acknowledged|=1u<<slot;
    }
    update(g);return 1;
}
int np_host_gate_packet(NpHostGate *g,NpHostPacket *out){
    if(!g||!g->enabled||!out||g->observer)return 0;
    memset(out,0,sizeof *out);out->slot=(uint8_t)g->local;
    if(g->error){out->epoch=NP_HOST_ERROR_MAGIC;out->a=g->error;return 1;}
    NpHostPeer *p=g->peers+g->local;unsigned count=p->chunks+(g->match.sealed?4:0);
    unsigned next=g->cursor++%count;
    if(next<p->chunks){out->epoch=NP_HOST_CAPS_MAGIC;out->flags=(uint8_t)next;
        out->a=word(p->wire+next*12);out->b=word(p->wire+next*12+4);out->seen=word(p->wire+next*12+8);
    }else{next-=p->chunks;out->epoch=NP_HOST_SELECT_MAGIC;out->flags=(uint8_t)next;
        out->a=word(g->match.selected.identity+next*8);out->b=word(g->match.selected.identity+next*8+4);out->seen=g->confirmed;}
    return 1;
}
int np_host_gate_ready(const NpHostGate *g){return g&&!g->error&&(!g->enabled||(psx_host_match_ready(&g->match)&&((g->acknowledged|(1u<<g->local))&g->occupied)==g->occupied));}
void np_host_gate_deadline(NpHostGate *g,uint32_t now){
    if(!g||!g->enabled||g->error||np_host_gate_ready(g))return;
    if(!g->clock_started){g->clock_started=1;g->start_ms=now;}
    else if(now-g->start_ms>=(g->observer?30000u:15000u))g->error=NP_HOST_TIMEOUT;
}
const char *np_host_gate_error(const NpHostGate *g){
    static const char *const names[]={"host_profile_agreement","host_profile_invalid","host_profile_conflict","host_profile_no_common","host_profile_artifact_mismatch","host_profile_confirmation_mismatch","host_profile_remote_rejected","host_profile_missing_peer_timeout","host_profile_startup_missing","host_profile_spectator_unsupported","host_profile_deferred_overflow"};
    return names[g&&g->error<sizeof names/sizeof names[0]?g->error:NP_HOST_BAD_DATA];
}
int np_host_gate_defer(NpHostGate *g,const NpHostControl *p){
    if(!g||!p||g->error)return 0;
    if(p->from<0||p->from>=9||!(g->occupied&(1u<<p->from))){g->error=NP_HOST_BAD_DATA;return 0;}
    if(p->op>6){g->error=NP_HOST_BAD_DATA;return 0;}
    if(g->deferred_count==64){g->error=NP_HOST_EARLY_CONTROL;return 0;}
    g->deferred[(g->deferred_head+g->deferred_count++)%64]=*p;return 1;
}
int np_host_gate_take_deferred(NpHostGate *g,NpHostControl *p){
    if(!g||!p||!np_host_gate_ready(g)||!g->deferred_count)return 0;
    *p=g->deferred[g->deferred_head];g->deferred_head=(g->deferred_head+1)%64;--g->deferred_count;return 1;
}
