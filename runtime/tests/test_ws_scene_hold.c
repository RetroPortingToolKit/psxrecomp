#include "ws_scene_hold.h"
#undef NDEBUG /* the Release test target must execute its assertions */
#include <assert.h>
#include <stdio.h>

int main(void) {
    WsSceneHold s;
    ws_scene_hold_reset(&s);
    assert(ws_scene_hold_classify(&s,2,0,0,512)==0); /* normal world */
    for (int i=0;i<300;++i)
        assert(ws_scene_hold_classify(&s,1,1,0,512)==0); /* held loading */
    assert(ws_scene_hold_classify(&s,2,1,0,512)==0); /* backbuffer ready */
    assert(ws_scene_hold_classify(&s,2,1,0,512)==0); /* still same frontbuffer */
    assert(ws_scene_hold_classify(&s,2,1,0,0)==1); /* real title becomes visible */
    assert(ws_scene_hold_classify(&s,2,0,0,512)==0); /* no hold on later flips */
    assert(ws_scene_hold_classify(&s,1,1,0,512)==0);
    assert(ws_scene_hold_classify(&s,0,1,0,512)==1); /* immediate in-place release */
    assert(ws_scene_hold_classify(&s,1,0,0,512)==1); /* retained 4:3 stays 4:3 */
    assert(ws_scene_hold_classify(&s,0,0,0,0)==0);
    assert(ws_scene_hold_classify(&s,1,0,1,0)==1); /* FMV veto */
    assert(ws_scene_hold_classify(&s,2,0,1,0)==1);
    assert(ws_scene_hold_classify(&s,0,0,0,0)==0);
    assert(ws_scene_hold_classify(&s,1,1,0,0)==0);
    ws_scene_hold_reset(&s); /* unrelated pre-restore margins cannot survive */
    assert(ws_scene_hold_classify(&s,2,1,0,0)==1);
    assert(ws_scene_hold_classify(&s,0,0,0,0)==0);
    puts("PASS retained scene, delayed flip, immediate release, FMV and reset");
    return 0;
}
