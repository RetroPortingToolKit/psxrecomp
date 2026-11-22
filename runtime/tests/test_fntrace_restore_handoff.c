#include "fntrace.h"
#include <stdio.h>
#include <stdlib.h>

static int effects;
void dirty_ram_clear_image_baseline(void) { ++effects; }
void cdrom_notify_game_started(void) { ++effects; }
void boot_state_trigger_capture(const CPUState* cpu) { (void)cpu; ++effects; }
static void check(int ok, const char* message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main(void) {
    CPUState cpu = {0};
    fntrace_set_game_range(0x8006B58Cu, 0);
    fntrace_restore_game_started(1);
    fntrace_mark_game_started(&cpu);
    check(fntrace_is_game_started() && !effects, "cold game restore skips destructive handoff");
    fntrace_restore_game_started(0);
    check(!fntrace_is_game_started() && !effects, "pre-game restore resets warm latch");
    fntrace_mark_game_started(&cpu);
    check(effects == 3, "normal boot retains required handoff effects");
    fntrace_mark_game_started(&cpu);
    check(effects == 3, "handoff remains one-shot");
    puts("restored handoff checks passed");
    return 0;
}
