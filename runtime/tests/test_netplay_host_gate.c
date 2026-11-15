#include "netplay_host_gate.h"
#include "netplay_content_gate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define NOTE(g,p) np_host_gate_note(g,p,(p)->slot)
static NpHostGate a,b;
static PSXHostProfileRule rules[2];
static PSXHostProfileCatalog cat={rules,2};
static uint8_t artifact[32]={1},identity[32]={2};
static uint8_t nonce_a[16]={3},nonce_b[16]={4};
static void setup(int different,int no_common){
    memset(rules,0,sizeof rules);
    for(unsigned i=0;i<2;++i){PSXHostProfileRule *r=rules+i;strcpy(r->profile,i?"portable":"fast");
        r->revision=1;r->priority=i?1:2;r->qualification[0]=(uint8_t)(i+1);
        r->flags=i?PSX_HOST_PROFILE_PORTABLE:0;r->implementation_count=1;
        r->implementations[0].backend=i?1:2;r->implementations[0].architectures=(1u<<PSX_HOST_ARCH_COUNT)-1;
        r->implementations[0].required_features=i?0:PSX_HOST_AVX2;}
    PSXHostCPU cpu={PSX_HOST_ARCH_X64,1,PSX_HOST_SSE2|PSX_HOST_AVX2};PSXHostCapabilities c,d;
    CHECK(psx_host_caps_init(&c,&cpu,3,3,artifact));cpu.features=PSX_HOST_SSE2;
    CHECK(psx_host_caps_init(&d,&cpu,3,1,artifact));
    CHECK(psx_host_caps_offer(&c,rules,2));if(!no_common)CHECK(psx_host_caps_offer(&c,rules+1,1));
    CHECK(psx_host_caps_offer(&d,rules+1,1));if(different)d.build[0]^=2;
    ++nonce_a[0];++nonce_b[0];
    CHECK(np_host_gate_init(&a,&c,&cat,0x101,0,identity,nonce_a));
    CHECK(np_host_gate_init(&b,&d,&cat,0x101,8,identity,nonce_b));
}
static void caps(NpHostGate *from,NpHostGate *to){
    NpHostPacket packets[NP_HOST_FRAGMENTS];unsigned count=from->peers[from->local].chunks;
    for(unsigned i=0;i<count;++i)CHECK(np_host_gate_packet(from,packets+i));
    for(unsigned i=count;i>0;--i){CHECK(NOTE(to,packets+i-1));CHECK(NOTE(to,packets+i-1));}
}
static void finish(unsigned drop){
    for(unsigned t=0;t<1000 && (!np_host_gate_ready(&a)||!np_host_gate_ready(&b));++t){
        NpHostPacket p;
        CHECK(np_host_gate_packet(&a,&p));if(!drop||t%drop)CHECK(NOTE(&b,&p));
        CHECK(np_host_gate_packet(&b,&p));if(!drop||(t+1)%drop)CHECK(NOTE(&a,&p));
        CHECK(!a.error&&!b.error);
    }
    CHECK(np_host_gate_ready(&a)&&np_host_gate_ready(&b));
    CHECK(!memcmp(a.match.selected.identity,b.match.selected.identity,32));
}
static void reorder_loss_and_deferred(void){
    setup(0,0);CHECK(!np_host_gate_ready(&a)&&!np_host_gate_ready(&b));caps(&a,&b);caps(&b,&a);
    CHECK(a.match.sealed&&b.match.sealed&&!np_host_gate_ready(&a));CHECK(!strcmp(a.match.selected.profile,"portable"));
    /* Hold B's complete-confirmation ACK from A while allowing the reverse.
     * B can become ready while A remains pending; no data may apply at A. */
    for(unsigned t=0;t<200 && !np_host_gate_ready(&b);++t){NpHostPacket p;
        np_host_gate_packet(&a,&p);NOTE(&b,&p);
        np_host_gate_packet(&b,&p);if(p.epoch==NP_HOST_SELECT_MAGIC)p.seen=0;NOTE(&a,&p);}
    CHECK(np_host_gate_ready(&b)&&!np_host_gate_ready(&a));
    NpHostControl p={7,10,20,30,0,1,2,8},out;
    CHECK(np_host_gate_defer(&a,&p));p.epoch=8;p.op=3;CHECK(np_host_gate_defer(&a,&p));
    CHECK(!np_host_gate_take_deferred(&a,&out));finish(3);
    CHECK(np_host_gate_take_deferred(&a,&out)&&out.epoch==7&&out.op==1&&out.from==8&&out.slot==0);
    CHECK(np_host_gate_take_deferred(&a,&out)&&out.epoch==8&&out.op==3&&out.from==8&&out.slot==0);
    CHECK(!np_host_gate_take_deferred(&a,&out));
    PSXHostSelection chosen=a.match.selected;finish(0);CHECK(!memcmp(&chosen,&a.match.selected,sizeof chosen));
}
static void invalid(void){
    setup(0,0);NpHostPacket p;np_host_gate_packet(&a,&p);NOTE(&b,&p);p.a^=1;
    NOTE(&b,&p);CHECK(b.error==NP_HOST_CONFLICT&&!np_host_gate_ready(&b));
    setup(0,0);np_host_gate_packet(&a,&p);p.flags=NP_HOST_FRAGMENTS;NOTE(&b,&p);CHECK(b.error==NP_HOST_BAD_DATA);
    setup(0,0);np_host_gate_packet(&a,&p);p.flags=52;NOTE(&b,&p);p.flags=0;NOTE(&b,&p);
    np_host_gate_packet(&a,&p);NOTE(&b,&p);np_host_gate_packet(&a,&p);NOTE(&b,&p);CHECK(b.error==NP_HOST_BAD_DATA);
    setup(1,0);caps(&b,&a);CHECK(a.error==NP_HOST_ARTIFACT);
    setup(0,1);caps(&a,&b);caps(&b,&a);CHECK(a.error==NP_HOST_REMOTE&&b.error==NP_HOST_NO_COMMON);
    setup(0,0);caps(&a,&b);caps(&b,&a);
    p=(NpHostPacket){NP_HOST_SELECT_MAGIC,1,2,0,8,0};NOTE(&a,&p);
    for(unsigned i=1;i<4;++i){p.flags=(uint8_t)i;NOTE(&a,&p);}CHECK(a.error==NP_HOST_CONFIRM);
    setup(0,0);np_host_gate_deadline(&a,100);np_host_gate_deadline(&a,15099);CHECK(!a.error&&!np_host_gate_ready(&a));
    np_host_gate_deadline(&a,15100);CHECK(a.error==NP_HOST_TIMEOUT);
    setup(0,0);NpHostControl c={1,2,3,4,8,1,0,8};
    for(unsigned i=0;i<64;++i)CHECK(np_host_gate_defer(&a,&c));
    CHECK(!np_host_gate_defer(&a,&c));CHECK(a.error==NP_HOST_EARLY_CONTROL);
    CHECK(!np_host_gate_ready(&a));
    setup(0,0);np_host_gate_packet(&b,&p);CHECK(np_host_gate_note(&a,&p,0));
    CHECK(a.error==NP_HOST_BAD_DATA&&!np_host_gate_ready(&a)); /* Claimed seat8, author0. */
    setup(0,0);caps(&a,&b);caps(&b,&a);p=(NpHostPacket){NP_HOST_SELECT_MAGIC,1,2,0,8,0};
    CHECK(np_host_gate_note(&a,&p,0));CHECK(a.error==NP_HOST_BAD_DATA); /* Wrong selection author too. */
    setup(0,0);c.op=255;CHECK(!np_host_gate_defer(&a,&c));CHECK(a.error==NP_HOST_BAD_DATA);
    setup(0,0);CHECK(!a.error&&!a.deferred_count); /* Fresh match discards prior state. */
}
static void maximal(void){
    PSXHostProfileRule many[8];memset(many,0,sizeof many);PSXHostProfileCatalog local={many,8};
    PSXHostCPU cpu={PSX_HOST_ARCH_UNKNOWN,0,0};PSXHostCapabilities c;
    CHECK(psx_host_caps_init(&c,&cpu,1,1,artifact));
    for(unsigned i=0;i<8;++i){snprintf(many[i].profile,32,"profile%u",i);many[i].revision=1;
        many[i].flags=PSX_HOST_PROFILE_PORTABLE;many[i].qualification[0]=(uint8_t)(i+1);
        many[i].implementation_count=1;many[i].implementations[0].backend=1;
        many[i].implementations[0].architectures=(1u<<PSX_HOST_ARCH_COUNT)-1;CHECK(psx_host_caps_offer(&c,many+i,1));}
    CHECK(np_host_gate_init(&a,&c,&local,0x101,0,identity,nonce_a));CHECK(np_host_gate_init(&b,&c,&local,0x101,8,identity,nonce_b));
    CHECK(a.peers[0].bytes==652&&a.peers[0].chunks==55);caps(&a,&b);caps(&b,&a);finish(4);
}
static void stale_match_replay(void){
    static NpHostGate old_peer;setup(0,0);finish(0);old_peer=b;
    uint8_t chosen[32];memcpy(chosen,a.match.selected.identity,32);
    setup(0,0); /* Same session/artifact/content, fresh local nonce. */
    for(unsigned i=0;i<200&&!a.error;++i){NpHostPacket p;np_host_gate_packet(&old_peer,&p);NOTE(&a,&p);}
    CHECK(!np_host_gate_ready(&a)&&a.error==NP_HOST_CONFIRM);
    CHECK(memcmp(chosen,a.match.selected.identity,32));
    setup(0,0);CHECK(!a.error&&!a.deferred_count);finish(0);
}
static void unknown_remote_offer(void){
    setup(0,0);PSXHostProfileRule remote_rules[2]={rules[1],rules[0]};
    memset(remote_rules[1].profile,0,32);strcpy(remote_rules[1].profile,"unknown-fast");remote_rules[1].qualification[0]=9;
    PSXHostProfileCatalog remote={remote_rules,2};PSXHostCapabilities c=b.capabilities[8];
    c.cpu.features|=PSX_HOST_AVX2;c.available_backends=3;
    CHECK(psx_host_caps_offer(&c,remote_rules+1,2));
    CHECK(np_host_gate_init(&b,&c,&remote,0x101,8,identity,nonce_b));
    finish(0);CHECK(!strcmp(a.match.selected.profile,"portable"));
}
static void old_protocol_rejects(void){
    const char *prior="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char required[65],again[65];CHECK(psx_host_required_content_identity(prior,artifact,required));
    CHECK(strcmp(prior,required));strcpy(again,prior);CHECK(psx_host_required_content_identity(again,artifact,again));
    CHECK(!strcmp(required,again));NpContentGate old,current;
    CHECK(np_content_init(&old,prior,3,0));CHECK(np_content_init(&current,required,3,1));
    for(unsigned i=0;i<4;++i){np_content_note(&old,1,i,current.words[i*2],current.words[i*2+1],current.matched);
        np_content_note(&current,0,i,old.words[i*2],old.words[i*2+1],old.matched);}
    CHECK(old.mismatch&&current.mismatch&&!np_content_ready(&old)&&!np_content_ready(&current));
    uint8_t other[32]={3};CHECK(psx_host_required_content_identity(prior,other,again));CHECK(strcmp(required,again));
    uint8_t zero[32]={0};CHECK(!psx_host_required_content_identity(prior,zero,again));
    CHECK(!psx_host_required_content_identity("v11",artifact,again));
}
/* Synthetic rules remain fixture policy, never a production qualification. */
static void observer(void){
    static NpHostGate viewer;NpHostPacket p;
    setup(0,0);rules[1].flags|=PSX_HOST_PROFILE_OBSERVER;
    PSXHostCapabilities ca=a.capabilities[0],cb=b.capabilities[8];
    CHECK(np_host_gate_init(&a,&ca,&cat,0x101,0,identity,nonce_a));
    CHECK(np_host_gate_init(&b,&cb,&cat,0x101,8,identity,nonce_b));finish(0);
    uint8_t frozen[32];memcpy(frozen,a.match.selected.identity,32);
    CHECK(np_host_gate_observer_init(&viewer,&cb,&cat,0x101,identity));
    CHECK(!np_host_gate_packet(&viewer,&p));
    for(unsigned i=0;i<500&&!np_host_gate_ready(&viewer);++i){
        CHECK(np_host_gate_packet(&a,&p));CHECK(np_host_gate_note(&viewer,&p,0));
        CHECK(np_host_gate_packet(&b,&p));CHECK(np_host_gate_note(&viewer,&p,8));
        CHECK(!viewer.error);
    }
    CHECK(np_host_gate_ready(&viewer)&&viewer.confirmed==0x101);
    CHECK(!np_host_gate_packet(&viewer,&p)&&viewer.local==9);
    CHECK(!memcmp(frozen,viewer.match.selected.identity,32));
    CHECK(!memcmp(frozen,a.match.selected.identity,32));
    CHECK(np_host_gate_observer_init(&viewer,&cb,&cat,0x101,identity));
    CHECK(np_host_gate_packet(&a,&p));CHECK(np_host_gate_note(&viewer,&p,8));
    CHECK(viewer.error==NP_HOST_BAD_DATA);
    cb.build[0]^=1;CHECK(np_host_gate_observer_init(&viewer,&cb,&cat,0x101,identity));
    for(unsigned i=0;i<200&&!viewer.error;++i){np_host_gate_packet(&a,&p);np_host_gate_note(&viewer,&p,0);}
    CHECK(viewer.error==NP_HOST_ARTIFACT);cb.build[0]^=1;
    rules[1].flags&=~PSX_HOST_PROFILE_OBSERVER;
    CHECK(np_host_gate_observer_init(&viewer,&cb,&cat,0x101,identity));
    for(unsigned i=0;i<200&&!viewer.error;++i){np_host_gate_packet(&a,&p);np_host_gate_note(&viewer,&p,0);np_host_gate_packet(&b,&p);np_host_gate_note(&viewer,&p,8);}
    CHECK(viewer.error==NP_HOST_NO_COMMON);
    NpContentGate content;const char *hex="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(np_content_observer_init(&content,hex,0x101));CHECK(!np_content_ready(&content));
    for(unsigned i=0;i<4;++i)for(unsigned seat=0;seat<=8;seat+=8)
        np_content_note(&content,seat,i,content.words[2*i],content.words[2*i+1],0x101);
    CHECK(np_content_ready(&content)&&content.local==9&&content.matched==0x101);
}
int main(void){observer();reorder_loss_and_deferred();invalid();maximal();old_protocol_rejects();stale_match_replay();unknown_remote_offer();puts("host wire reorder/loss/immutable agreement, deferred rollback and terminal failures PASS");return 0;}
