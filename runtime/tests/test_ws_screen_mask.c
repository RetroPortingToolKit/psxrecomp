#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "ws_screen_mask.h"
#include "ws_hud_anchor.h"
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
    /* Same packet address with different geometry/colour must lose its tag. */
    WsHudAnchorTag tags[WS_HUD_ANCHOR_TABLE_SIZE]={0};
    uint32_t words[5]={0x2a7b7b7b,0x0000ffa0,0xa0,0xf0ffa0,0xf00044};
    WsPrepassPacketGuard guard=ws_prepass_packet_guard(words,5);
    ws_hud_anchor_insert(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,0,&guard,20);
    assert(ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
    words[2]++;assert(!ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
    words[2]--;ws_hud_anchor_clear(tags,WS_HUD_ANCHOR_TABLE_SIZE);
    assert(!ws_hud_anchor_lookup(tags,WS_HUD_ANCHOR_TABLE_SIZE,0xa68b4,words,5,20,0));
}
