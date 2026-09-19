/* One-shot SHA-256, based directly on FIPS 180-4. Public domain. */
#include "psx_sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void block(uint32_t h[8], const uint8_t b[64]) {
    uint32_t w[64];
    for (int i=0;i<16;i++) w[i]=((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|
                                  ((uint32_t)b[i*4+2]<<8)|(uint32_t)b[i*4+3];
    for (int i=16;i<64;i++) {
        uint32_t s0=rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=h[0],c0=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],x=h[7];
    for (int i=0;i<64;i++) {
        uint32_t s1=rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
        uint32_t t1=x+s1+((e&f)^((~e)&g))+K[i]+w[i];
        uint32_t s0=rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
        uint32_t t2=s0+((a&c0)^(a&c)^(c0&c));
        x=g;g=f;f=e;e=d+t1;d=c;c=c0;c0=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=c0;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=x;
}

void psx_sha256_init(psx_sha256_ctx* ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(ctx->h, initial, sizeof(initial));
    ctx->total = 0;
    ctx->buffered = 0;
}

void psx_sha256_update(psx_sha256_ctx* ctx, const uint8_t* data, size_t len) {
    ctx->total += len;
    if (ctx->buffered) {
        size_t take = 64 - ctx->buffered;
        if (take > len) take = len;
        memcpy(ctx->buffer + ctx->buffered, data, take);
        ctx->buffered += take; data += take; len -= take;
        if (ctx->buffered == 64) {
            block(ctx->h, ctx->buffer);
            ctx->buffered = 0;
        }
    }
    while (len >= 64) {
        block(ctx->h, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->buffer, data, len);
        ctx->buffered = len;
    }
}

void psx_sha256_final(psx_sha256_ctx* ctx, uint8_t out[32]) {
    uint8_t tail[128];
    size_t n=ctx->buffered,total=(n<56)?64:128;
    memcpy(tail,ctx->buffer,n); tail[n]=0x80;
    memset(tail+n+1,0,total-n-1-8);
    uint64_t bits=ctx->total*8;
    for (int i=0;i<8;i++) tail[total-1-i]=(uint8_t)(bits>>(i*8));
    block(ctx->h,tail); if (total==128) block(ctx->h,tail+64);
    for (int i=0;i<8;i++) {
        out[i*4]=(uint8_t)(ctx->h[i]>>24); out[i*4+1]=(uint8_t)(ctx->h[i]>>16);
        out[i*4+2]=(uint8_t)(ctx->h[i]>>8); out[i*4+3]=(uint8_t)ctx->h[i];
    }
}

void psx_sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    psx_sha256_ctx ctx;
    psx_sha256_init(&ctx);
    psx_sha256_update(&ctx, data, len);
    psx_sha256_final(&ctx, out);
}
