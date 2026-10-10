/* [widescreen] scene_predicate_per_frame (ws_scene_cache.h): a predicate
 * that flips inside a frame keeps the frame's first answer until the next
 * frame; a flip between frames is seen on the next frame's first call; an
 * invalidation (savestate load, reset, a new predicate) re-evaluates at once;
 * with the option off every call evaluates, exactly as without the cache. */
#include "ws_scene_cache.h"

#include <stdio.h>

static int s_value, s_calls;
static int pred(void) { s_calls++; return s_value; }

static int fails;
static void ok(int c, const char *w) { printf("  %s  %s\n", c ? "ok  " : "FAIL", w); if (!c) fails++; }

int main(void) {
    WsSceneCache c = {0};

    s_value = 1; s_calls = 0;
    ok(ws_scene_cache_get(&c, 1, 10, pred) == 1, "frame 10, first call: evaluated (1)");
    s_value = 0;   /* the scene word changes mid-frame */
    ok(ws_scene_cache_get(&c, 1, 10, pred) == 1, "frame 10, after a mid-frame flip: still 1");
    ok(ws_scene_cache_get(&c, 1, 10, pred) == 1 && s_calls == 1,
       "frame 10: one evaluation for the whole frame");
    ok(ws_scene_cache_get(&c, 1, 11, pred) == 0 && s_calls == 2,
       "frame 11: the flip is seen on its first call");
    s_value = 1;
    ok(ws_scene_cache_get(&c, 1, 11, pred) == 0, "frame 11: a second flip waits for frame 12");
    ok(ws_scene_cache_get(&c, 1, 12, pred) == 1, "frame 12: seen");

    s_value = 0;
    ws_scene_cache_invalidate(&c);   /* savestate load */
    ok(ws_scene_cache_get(&c, 1, 12, pred) == 0, "invalidation re-evaluates in the same frame");

    s_calls = 0;
    s_value = 1;
    ok(ws_scene_cache_get(&c, 0, 12, pred) == 1, "off: evaluated");
    s_value = 0;
    ok(ws_scene_cache_get(&c, 0, 12, pred) == 0 && s_calls == 2,
       "off: a mid-frame flip is seen at once, every call evaluates");

    WsSceneCache w = {0};
    s_value = 1;
    ok(ws_scene_cache_get(&w, 1, 0xFFFFFFFFu, pred) == 1, "frame counter wrap: evaluated");
    s_value = 0;
    ok(ws_scene_cache_get(&w, 1, 0u, pred) == 0, "frame counter wrap: next frame re-evaluates");

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
