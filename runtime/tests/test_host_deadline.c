#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t clock_ticks, clock_step=10, clock_frequency=1000000;
static unsigned sleeps, largest_sleep, overshoot, interrupt_sleep;
uint64_t SDL_GetPerformanceCounter(void) {
    uint64_t sample=clock_ticks;clock_ticks+=clock_step;return sample;
}
uint64_t SDL_GetPerformanceFrequency(void) { return clock_frequency; }
void psx_host_sleep_micros(unsigned us) {
    ++sleeps;if(us>largest_sleep)largest_sleep=us;
    assert(us<=1000000);
    clock_ticks+=(interrupt_sleep?us/2u:us)+overshoot;
}
void psx_host_sleep_ms(unsigned ms) { psx_host_sleep_micros(ms*1000u); }
/* Same wait loop and frame-pacer body, deterministic clock/timer providers. */
#define PSX_HOST_DEADLINE_TESTING 1
#include "../src/host_deadline.c"
#include "../src/frame_pacing.c"
static void reset(uint64_t now) {
    clock_ticks=now;clock_step=10;clock_frequency=1000000;
    sleeps=largest_sleep=overshoot=interrupt_sleep=0;
}
int main(void) {
    assert(!psx_host_deadline_sleep_us(100,99,1000000,16667));
    assert(!psx_host_deadline_sleep_us(100,100,1000000,16667));
    assert(!psx_host_deadline_sleep_us(0,10000,0,16667));
    assert(!psx_host_deadline_sleep_us(0,10000,1000000,0));
    assert(!psx_host_deadline_sleep_us(0,300,1000000,16667));
    assert(psx_host_deadline_sleep_us(0,10000,1000000,16667)==9800);
    assert(psx_host_deadline_sleep_us(0,UINT64_MAX,1000000,16667)<=16667);
    assert(psx_host_deadline_sleep_us(0,UINT64_MAX,UINT64_MAX,UINT64_MAX)<=1000000);
    reset(1000);assert(!psx_host_wait_until(0,1000000,16667));assert(!sleeps);
    assert(!psx_host_wait_until(999,1000000,16667));assert(!sleeps);
    reset(1000);uint64_t elapsed=psx_host_wait_until(10000,1000000,16667);
    assert(sleeps && elapsed>=9000 && elapsed<9100 && largest_sleep<9000);
    reset(1000);interrupt_sleep=1;
    elapsed=psx_host_wait_until(10000,1000000,16667);
    assert(sleeps>1 && elapsed>=9000 && elapsed<9100);
    reset(1000);overshoot=3000;
    elapsed=psx_host_wait_until(10000,1000000,16667);
    assert(sleeps==1 && elapsed>=9000 && elapsed<12000);
    reset(1000);elapsed=psx_host_wait_until(UINT64_MAX,1000000,16667);
    assert(elapsed>=16667 && elapsed<16767); /* bounded poisoned deadline */
    reset(1000);FramePacer p={0};
    frame_pacer_wait(&p,1000.0/60.0);assert(!sleeps && p.next_deadline==17666);
    frame_pacer_wait(&p,1000.0/60.0);assert(sleeps && p.next_deadline==34332);
    assert(clock_ticks>=17666 && clock_ticks<17766);
    /* A finite stall keeps its debt; a sustained stall re-anchors. */
    clock_ticks=p.next_deadline+10000;sleeps=0;
    frame_pacer_wait(&p,1000.0/60.0);assert(!sleeps && p.next_deadline==50998);
    clock_ticks=p.next_deadline+13u*16666u;uint64_t anchor=clock_ticks;
    frame_pacer_wait(&p,1000.0/60.0);assert(!sleeps && p.next_deadline==anchor+16666u);
    p.next_deadline=0;anchor=clock_ticks;
    frame_pacer_wait(&p,1000.0/50.0);assert(!sleeps && p.next_deadline==anchor+20000u);
    puts("deadline progress, bounds, interrupted/overshooting timers, debt and reset passed");
    return 0;
}
