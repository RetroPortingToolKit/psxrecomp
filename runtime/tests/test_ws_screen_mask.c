#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "ws_screen_mask.h"
#include "ws_hud_anchor.h"
#include "ws_radial_screen_mask.h"
int main(void) {
    int32_t x[4]={-96,160,-96,68},y[4]={0,0,240,240};
    WsScreenMaskBand b;
    for(int margin=0;margin<16000;margin+=53) {
        int result=ws_screen_mask_band(x,y,320,240,margin,&b);
        assert(result==(margin>96));
        if(result) assert(b.x==-margin && b.x+b.w==-96 && b.y==0 && b.h==240);
    }
    x[0]=160;x[1]=416;x[2]=308;x[3]=416;
    assert(ws_screen_mask_band(x,y,320,240,694,&b));
    assert(b.x==416 && b.w==598);
    x[3]=415; assert(!ws_screen_mask_band(x,y,320,240,694,&b));
    x[3]=416;y[3]=239;assert(!ws_screen_mask_band(x,y,320,240,694,&b));
    WsScreenMaskBand bands[2];
    x[0]=x[2]=0; x[1]=x[3]=512;
    y[0]=y[1]=0; y[2]=y[3]=25;
    for (int margin=0; margin<16000; margin+=53) {
        int count=ws_screen_mask_bands(x,y,512,240,margin,bands);
        assert(count==(margin ? 2 : 0));
        if (count) {
            assert(bands[0].x==-margin && bands[0].w==margin);
            assert(bands[1].x==512 && bands[1].w==margin);
            assert(bands[0].y==0 && bands[0].h==25);
            assert(bands[1].y==0 && bands[1].h==25);
        }
    }
    y[0]=y[1]=215; y[2]=y[3]=240;
    assert(ws_screen_mask_bands(x,y,512,240,170,bands)==2);
    assert(bands[0].y==215 && bands[0].h==25);
    /* Already extended edges need no second draw; one-sided overlap is safe. */
    x[0]=x[2]=-200; x[1]=x[3]=600;
    assert(ws_screen_mask_bands(x,y,512,240,170,bands)==1);
    assert(bands[0].x==600 && bands[0].w==82);
    x[1]=x[3]=700;
    assert(!ws_screen_mask_bands(x,y,512,240,170,bands));
    /* A world quad, middle strip, empty panel or full-screen blend stays alone. */
    x[0]=x[2]=0; x[1]=x[3]=512;
    y[0]=y[1]=20; y[2]=y[3]=40;
    assert(!ws_screen_mask_bands(x,y,512,240,170,bands));
    y[0]=y[1]=0; y[2]=y[3]=240;
    assert(!ws_screen_mask_bands(x,y,512,240,170,bands));
    y[2]=y[3]=0;
    assert(!ws_screen_mask_bands(x,y,512,240,170,bands));
    y[2]=y[3]=25; x[3]=511;
    assert(!ws_screen_mask_bands(x,y,512,240,170,bands));
    /* Same packet address with different geometry/colour must lose its tag. */
    WsHudAnchorTag tags[WS_HUD_ANCHOR_TABLE_SIZE]={0};
    uint32_t words[5]={0x2a7b7b7b,0x0000ffa0,0xa0,0xf0ffa0,0xf00044};
    WsPrepassPacketGuard guard=ws_prepass_packet_guard(words,5);
    ws_hud_anchor_insert(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,0,&guard,20);
    assert(ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
    /* Expanded render memory must not alias the same packet offset at 2 MiB. */
    assert(!ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0x2a68b4,words,5,20,0));
    ws_hud_anchor_insert(tags,WS_HUD_ANCHOR_TABLE_SIZE,0x2a68b4,0,&guard,20);
    assert(ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0x2a68b4,words,5,20,0));
    assert(ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
    words[2]++;assert(!ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
    words[2]--;ws_hud_anchor_clear(tags,WS_HUD_ANCHOR_TABLE_SIZE);
    assert(!ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
    WsRadialScreenMaskTag radial[WS_RADIAL_MASK_TAG_COUNT]={0};
    float scale=0;
    ws_radial_mask_insert(radial,0x4a68b4,&guard,20,1.5f);
    assert(ws_radial_mask_lookup(radial,0x4a68b4,words,5,22,&scale));
    assert(scale==1.5f);
    assert(!ws_radial_mask_lookup(radial,0x2a68b4,words,5,22,&scale));
    assert(!ws_radial_mask_lookup(radial,0x4a68b4,words,5,23,&scale));
    words[1]++;
    assert(!ws_radial_mask_lookup(radial,0x4a68b4,words,5,20,&scale));
    words[1]--;
    ws_radial_mask_insert(radial,0x4a68b4,&guard,20,NAN);
    assert(ws_radial_mask_lookup(radial,0x4a68b4,words,5,20,&scale));
    assert(scale==1.5f);
    int32_t rx[4]={256,356,-128,640}, ry[4]={120,170,-72,312};
    ws_radial_mask_transform(rx,ry,512,240,1.f);
    assert(rx[1]==356 && ry[1]==170);
    ws_radial_mask_transform(rx,ry,512,240,1.5f);
    assert(rx[0]==256 && ry[0]==120);
    assert(rx[1]==406 && ry[1]==195);
    assert(rx[2]==-320 && ry[2]==-168);
    assert(rx[3]==832 && ry[3]==408);
    ws_radial_mask_clear(radial);
    assert(!ws_radial_mask_lookup(radial,0x4a68b4,words,5,20,&scale));
}
