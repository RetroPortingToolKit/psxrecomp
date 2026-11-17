#include "netplay_host_profile.h"
#include "execution_profile.h"
#include "psx_sha256.h"
#include <string.h>

static int nonzero(const uint8_t *p){unsigned n=0;for(unsigned i=0;i<32;++i)n|=p[i];return n!=0;}
static int name_ok(const char *p){
    unsigned end=32;
    for(unsigned i=0;i<32;++i){unsigned c=(uint8_t)p[i];if(!c){end=i;break;}
        if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_'))return 0;}
    if(!end||end==32)return 0;
    for(unsigned i=end;i<32;++i)if(p[i])return 0;
    return 1;
}
static int one_bit(uint32_t x){return x && !(x&(x-1));}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
void psx_host_cpu_unknown(PSXHostCPU *out){
    if(!out)return;
    memset(out,0,sizeof *out);
#if defined(__x86_64__) || defined(_M_X64)
    out->architecture=PSX_HOST_ARCH_X64;
#elif defined(__i386__) || defined(_M_IX86)
    out->architecture=PSX_HOST_ARCH_X86;
#elif defined(__aarch64__) || defined(_M_ARM64)
    out->architecture=PSX_HOST_ARCH_ARM64;
#elif defined(__arm__) || defined(_M_ARM)
    out->architecture=PSX_HOST_ARCH_ARM32;
#elif defined(__powerpc__) || defined(__powerpc64__)
    out->architecture=PSX_HOST_ARCH_PPC;
#elif defined(__loongarch__)
    out->architecture=PSX_HOST_ARCH_LOONGARCH;
#elif defined(__riscv)
    out->architecture=PSX_HOST_ARCH_RISCV;
#endif
}
uint32_t psx_host_compiled_backend(void){
    return PSX_EXECUTION_ENHANCED?PSX_HOST_BACKEND_ENHANCED:PSX_HOST_BACKEND_REFERENCE;
}
static int base_ok(const PSXHostCapabilities *c){
    if(!c || c->cpu.architecture>=PSX_HOST_ARCH_COUNT || c->cpu.features_known>1 ||
       (c->cpu.features&~(uint64_t)PSX_HOST_FEATURES_ALL) ||
       (!c->cpu.features_known && c->cpu.features) || !c->compiled_backends ||
       (c->compiled_backends&~PSX_HOST_BACKENDS_ALL) ||
       (c->available_backends&~c->compiled_backends) || c->offer_count>PSX_HOST_MAX_OFFERS || !nonzero(c->build))return 0;
    uint64_t allowed=0;
    if(c->cpu.architecture==PSX_HOST_ARCH_X86||c->cpu.architecture==PSX_HOST_ARCH_X64)allowed=127;
    if(c->cpu.architecture==PSX_HOST_ARCH_ARM32||c->cpu.architecture==PSX_HOST_ARCH_ARM64)allowed=PSX_HOST_NEON;
    if(c->cpu.architecture==PSX_HOST_ARCH_PPC)allowed=PSX_HOST_ALTIVEC;
    if(c->cpu.architecture==PSX_HOST_ARCH_LOONGARCH)allowed=PSX_HOST_LSX|PSX_HOST_LASX;
    if(c->cpu.features&~allowed)return 0;
    for(unsigned i=0;i<c->offer_count;++i){const PSXHostOffer *o=c->offers+i;
        if(!name_ok(o->profile)||!o->revision||!nonzero(o->qualification)||
           !one_bit(o->backend)||!(o->backend&c->available_backends))return 0;
        for(unsigned j=0;j<i;++j)if(!memcmp(o->profile,c->offers[j].profile,32))return 0;
    }
    return 1;
}
static int rule_ok(const PSXHostProfileRule *r){
    if(!r||!name_ok(r->profile)||!r->revision||!nonzero(r->qualification)||
       (r->flags&~(PSX_HOST_PROFILE_PORTABLE|PSX_HOST_PROFILE_DIRECT_LOOPBACK|PSX_HOST_PROFILE_OBSERVER))||!r->implementation_count||
       r->implementation_count>PSX_HOST_MAX_IMPLEMENTATIONS)return 0;
    for(unsigned i=0;i<r->implementation_count;++i){const PSXHostImplementation *p=r->implementations+i;
        if(!one_bit(p->backend)||(p->backend&~PSX_HOST_BACKENDS_ALL)||!p->architectures||
           (p->architectures&~((1u<<PSX_HOST_ARCH_COUNT)-1))||
           (p->required_features&~(uint64_t)PSX_HOST_FEATURES_ALL)||
           ((r->flags&PSX_HOST_PROFILE_PORTABLE)&&p->required_features))return 0;
        for(unsigned j=0;j<i;++j)if(p->backend==r->implementations[j].backend)return 0;
    }
    return 1;
}
static int eligible(const PSXHostCapabilities *c,const PSXHostProfileRule *r,uint32_t backend){
    if(!rule_ok(r)||!(backend&c->available_backends))return 0;
    for(unsigned i=0;i<r->implementation_count;++i){const PSXHostImplementation *p=r->implementations+i;
        if(p->backend==backend && (p->architectures&(1u<<c->cpu.architecture)) &&
           (!p->required_features || (c->cpu.features_known &&
              (c->cpu.features&p->required_features)==p->required_features)))return 1;
    }
    return 0;
}
int psx_host_caps_init(PSXHostCapabilities *c,const PSXHostCPU *cpu,uint32_t compiled,uint32_t available,const uint8_t build[32]){
    if(!c)return 0;
    memset(c,0,sizeof *c);if(!cpu||!build)return 0;
    c->cpu=*cpu;c->compiled_backends=compiled;c->available_backends=available;memcpy(c->build,build,32);
    if(!base_ok(c)){memset(c,0,sizeof *c);return 0;}return 1;
}
int psx_host_caps_offer(PSXHostCapabilities *c,const PSXHostProfileRule *r,uint32_t backend){
    if(!base_ok(c)||!eligible(c,r,backend)||c->offer_count==PSX_HOST_MAX_OFFERS)return 0;
    for(unsigned i=0;i<c->offer_count;++i)if(!memcmp(c->offers[i].profile,r->profile,32))return 0;
    PSXHostOffer *o=c->offers+c->offer_count++;memset(o,0,sizeof *o);
    memcpy(o->profile,r->profile,32);o->revision=r->revision;o->backend=backend;memcpy(o->qualification,r->qualification,32);return 1;
}
static int catalog_ok(const PSXHostProfileCatalog *cat){
    if(!cat||!cat->rules||!cat->count||cat->count>PSX_HOST_MAX_OFFERS)return 0;
    for(unsigned i=0;i<cat->count;++i){if(!rule_ok(cat->rules+i))return 0;
        for(unsigned j=0;j<i;++j)if(!memcmp(cat->rules[i].profile,cat->rules[j].profile,32))return 0;}
    return 1;
}
int psx_host_caps_validate(const PSXHostCapabilities *c,const PSXHostProfileCatalog *cat){
    if(!base_ok(c)||!catalog_ok(cat))return 0;
    for(unsigned i=0;i<c->offer_count;++i){const PSXHostOffer *o=c->offers+i;unsigned j;
        for(j=0;j<cat->count;++j)if(!memcmp(o->profile,cat->rules[j].profile,32))break;
        if(j==cat->count)return 0;
        const PSXHostProfileRule *r=cat->rules+j;
        if(o->revision!=r->revision||memcmp(o->qualification,r->qualification,32)||!eligible(c,r,o->backend))return 0;
    }return 1;
}
size_t psx_host_caps_encode(const PSXHostCapabilities *c,uint8_t *out,size_t capacity){
    if(!base_ok(c))return 0;
    size_t bytes=56+72*c->offer_count;if(!out)return bytes;
    if(capacity<bytes)return 0;
    memcpy(out,"PCAP",4);out[4]=1;out[5]=(uint8_t)c->cpu.architecture;out[6]=(uint8_t)c->cpu.features_known;out[7]=(uint8_t)c->offer_count;
    put32(out+8,(uint32_t)c->cpu.features);put32(out+12,(uint32_t)(c->cpu.features>>32));
    put32(out+16,c->compiled_backends);put32(out+20,c->available_backends);memcpy(out+24,c->build,32);
    for(unsigned i=0;i<c->offer_count;++i){uint8_t *p=out+56+i*72;const PSXHostOffer *o=c->offers+i;
        memcpy(p,o->profile,32);put32(p+32,o->revision);put32(p+36,o->backend);memcpy(p+40,o->qualification,32);}
    return bytes;
}
int psx_host_caps_decode(PSXHostCapabilities *out,const uint8_t *data,size_t bytes){
    if(!out)return 0;
    memset(out,0,sizeof *out);
    if(!data||bytes<56||bytes>PSX_HOST_CAPS_MAX_BYTES||memcmp(data,"PCAP",4)||data[4]!=1||
       data[7]>PSX_HOST_MAX_OFFERS||bytes!=56+72u*data[7])return 0;
    PSXHostCapabilities c;memset(&c,0,sizeof c);c.cpu.architecture=data[5];c.cpu.features_known=data[6];c.offer_count=data[7];
    c.cpu.features=get32(data+8)|((uint64_t)get32(data+12)<<32);c.compiled_backends=get32(data+16);c.available_backends=get32(data+20);memcpy(c.build,data+24,32);
    for(unsigned i=0;i<c.offer_count;++i){const uint8_t *p=data+56+i*72;PSXHostOffer *o=c.offers+i;
        memcpy(o->profile,p,32);o->revision=get32(p+32);o->backend=get32(p+36);memcpy(o->qualification,p+40,32);}
    if(!base_ok(&c))return 0;
    *out=c;return 1;
}
void psx_host_match_init(PSXHostMatch *m){if(m)memset(m,0,sizeof *m);}
static const PSXHostOffer *find_offer(const PSXHostCapabilities *c,const char *name){
    for(unsigned i=0;i<c->offer_count;++i)if(!memcmp(c->offers[i].profile,name,32))return c->offers+i;
    return NULL;
}
int psx_host_match_select(PSXHostMatch *m,const PSXHostCapabilities *peers,uint32_t count,const PSXHostProfileCatalog *cat,const uint8_t session[32]){
    if(!m)return PSX_HOST_INVALID;
    if(m->sealed||m->failed)return PSX_HOST_LOCKED;
    if(!peers||count<2||count>PSX_HOST_MAX_PEERS||!session||!nonzero(session)||!catalog_ok(cat))return PSX_HOST_INVALID;
    for(unsigned i=0;i<count;++i){if(!base_ok(peers+i))return PSX_HOST_INVALID;
        if(memcmp(peers[0].build,peers[i].build,32))return PSX_HOST_BUILD_MISMATCH;}
    const PSXHostProfileRule *best=NULL;
    for(unsigned i=0;i<cat->count;++i){const PSXHostProfileRule *r=cat->rules+i;unsigned p;
        for(p=0;p<count;++p){const PSXHostOffer *o=find_offer(peers+p,r->profile);
            if(!o||o->revision!=r->revision||memcmp(o->qualification,r->qualification,32)||
               !eligible(peers+p,r,o->backend))break;}
        if(p!=count)continue;
        if(!best||r->priority>best->priority||(r->priority==best->priority&&strcmp(r->profile,best->profile)<0))best=r;
    }
    if(!best)return PSX_HOST_NO_COMMON;
    PSXHostSelection s;memset(&s,0,sizeof s);memcpy(s.profile,best->profile,32);s.revision=best->revision;s.flags=best->flags;memcpy(s.qualification,best->qualification,32);
    psx_sha256_ctx sha;psx_sha256_init(&sha);const char salt[]="psx-qualified-host-profile-v1";
    uint8_t integers[12];put32(integers,s.revision);put32(integers+4,s.flags);put32(integers+8,count);
    psx_sha256_update(&sha,(const uint8_t *)salt,sizeof salt-1);psx_sha256_update(&sha,session,32);
    psx_sha256_update(&sha,peers[0].build,32);psx_sha256_update(&sha,(const uint8_t *)s.profile,32);
    psx_sha256_update(&sha,integers,sizeof integers);psx_sha256_update(&sha,s.qualification,32);psx_sha256_final(&sha,s.identity);
    m->selected=s;m->peer_count=count;m->sealed=1;return PSX_HOST_OK;
}
int psx_host_match_confirm(PSXHostMatch *m,uint32_t peer,const uint8_t identity[32]){
    if(!m||!m->sealed||m->failed||m->peer_count<2||m->peer_count>PSX_HOST_MAX_PEERS||peer>=m->peer_count||!identity)return 0;
    if(memcmp(identity,m->selected.identity,32)){m->failed=1;return 0;}
    m->confirmed|=1u<<peer;return 1;
}
int psx_host_match_ready(const PSXHostMatch *m){return m&&m->sealed&&!m->failed&&m->peer_count>=2&&m->peer_count<=PSX_HOST_MAX_PEERS&&m->confirmed==((1u<<m->peer_count)-1);}
int psx_host_required_content_identity(const char *prior,const uint8_t artifact[32],char out[65]){
    uint8_t base[32],digest[32];
    if(!out)return 0;
    if(!prior||strlen(prior)!=64||!artifact||!nonzero(artifact)){out[0]=0;return 0;}
    for(unsigned i=0;i<32;++i){unsigned value=0;
        for(unsigned j=0;j<2;++j){unsigned c=(uint8_t)prior[i*2+j],n;
            if(c>='0'&&c<='9')n=c-'0';else if(c>='a'&&c<='f')n=c-'a'+10;
            else if(c>='A'&&c<='F')n=c-'A'+10;else{out[0]=0;return 0;}
            value=(value<<4)|n;}
        base[i]=(uint8_t)value;
    }
    const char salt[]="psx-required-hostgate-v1";const char hex[]="0123456789abcdef";
    psx_sha256_ctx sha;psx_sha256_init(&sha);
    psx_sha256_update(&sha,(const uint8_t *)salt,sizeof salt-1);
    psx_sha256_update(&sha,base,32);psx_sha256_update(&sha,artifact,32);psx_sha256_final(&sha,digest);
    for(unsigned i=0;i<32;++i){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}out[64]=0;return 1;
}
