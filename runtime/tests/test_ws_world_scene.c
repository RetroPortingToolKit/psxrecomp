/* Exercise production classifier; section GC omits unrelated GPU execution. */
#include "../src/gpu.c"
#undef NDEBUG
#include <assert.h>

uint64_t s_frame_count;
static uint32_t state_word;
static int world, calls, fmv, hold;
uint32_t psx_read_word(uint32_t address) { (void)address; return state_word; }
int mdec_recently_active(uint32_t frames) { (void)frames; return fmv; }
static int world_scene(void) { ++calls; return world; }
static int retained_scene(void) { return hold; }
static int native_43(void) { ++s_frame_count; return gpu_ws_present_native_43(); }

int main(void) {
    uint32_t allowed = 0x100;
    gpu_ws_set_gameplay_state_gate(0x80001000u, &allowed, 1);
    world = 1;
    psx_mod_set_world_scene_predicate(world_scene);
    ws_mode = 0;
    assert(native_43() == 0 && calls == 0); /* stock is untouched */
    ws_mode = 1;
    assert(native_43() == 1 && calls == 0); /* squash does not consult mod */
    ws_mode = 2;
    assert(native_43() == 0 && calls > 0); /* overrides state AND 2D detector */
    world = 0;
    assert(native_43() == 1); /* false defers, never forces wide */
    gpu_ws_set_gte_game_mode(1);
    state_word = 0x100;
    assert(native_43() == 0); /* original allowlist still works */
    state_word = 0;
    assert(native_43() == 1);
    world = 1;
    fmv = 1;
    assert(native_43() == 1); /* streamed FMV veto */
    fmv = 0;
    display_depth = 1;
    assert(native_43() == 1); /* 24-bit FMV veto */
    display_depth = 0;
    assert(native_43() == 0);
    psx_mod_set_retained_scene_predicate(retained_scene);
    hold = 0;
    assert(native_43() == 0);
    world = 0;
    hold = 1;
    assert(native_43() == 0); /* old world retained during loading */
    hold = 0;
    assert(native_43() == 1); /* actual menu returns to 4:3 */
    psx_mod_set_retained_scene_predicate(NULL);
    world = 1;
    psx_mod_set_world_scene_predicate(NULL);
    assert(native_43() == 1); /* unregistration restores original policy */
    puts("PASS opt-in scope, world scene, defer, FMV veto, retained frame and unregister");
    return 0;
}
