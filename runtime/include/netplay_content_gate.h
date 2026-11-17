#ifndef PSX_NETPLAY_CONTENT_GATE_H
#define PSX_NETPLAY_CONTENT_GATE_H
#include <stdint.h>
#include <string.h>
#define NP_CONTENT_MAGIC 0x504D4431u /* PMD1: full 256-bit plan, four fragments */
typedef struct NpContentGate {
    uint32_t words[8], occupied, matched, acknowledged;
    uint8_t chunks[9], enabled, mismatch, local;
} NpContentGate;
static int np_content_init(NpContentGate *g, const char *hex, uint32_t occupied, int local) {
    unsigned i;
    memset(g, 0, sizeof(*g));
    if (!hex || !hex[0]) return 1;
    if (strlen(hex) != 64 || local < 0 || local >= 9 || (occupied & ~0x1ffu)) return 0;
    for (i=0;i<64;++i) {
        unsigned c=(unsigned char)hex[i], v;
        if(c>='0' && c<='9') v=c-'0';
        else if(c>='a' && c<='f') v=c-'a'+10;
        else if(c>='A' && c<='F') v=c-'A'+10;
        else return 0;
        g->words[i/8]=(g->words[i/8]<<4)|v;
    }
    g->enabled=1;g->local=(uint8_t)local;
    g->occupied=occupied|(1u<<local);g->matched=1u<<local;
    return 1;
}
static int np_content_observer_init(NpContentGate *g,const char *hex,uint32_t occupied){
    if(!np_content_init(g,hex,occupied,0))return 0;
    g->local=9;g->occupied=occupied;g->matched=0;return 1;
}
static void np_content_note(NpContentGate *g, unsigned slot, unsigned flags,
                            uint32_t a, uint32_t b, uint32_t seen) {
    unsigned chunk=flags&3u; uint32_t bit;
    if(!g->enabled || slot>=9 || slot==g->local) return;
    bit=1u<<slot;
    if(!(g->occupied&bit)) return;
    if((flags&0x80u) || a!=g->words[chunk*2] || b!=g->words[chunk*2+1]) {
        g->mismatch=1;return;
    }
    g->chunks[slot]|=(uint8_t)(1u<<chunk);
    if(g->chunks[slot]==15) g->matched|=bit;
    if((seen&g->occupied)==g->occupied) g->acknowledged|=bit;
}
static int np_content_ready(const NpContentGate *g) {
    return !g->enabled || (!g->mismatch && g->matched==g->occupied &&
        ((g->acknowledged|(1u<<g->local))&g->occupied)==g->occupied);
}
#endif
